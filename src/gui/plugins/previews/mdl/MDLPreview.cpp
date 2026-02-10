#include "MDLPreview.h"

#include <filesystem>
#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>

#include <kvpp/kvpp.h>
#include <mdlpp/mdlpp.h>
#include <QApplication>
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QFormLayout>
#include <QMessageBox>
#include <QMouseEvent>
#include <QFile>
#include <QPushButton>
#include <QSignalBlocker>
#include <QWidgetAction>
#include <QStackedLayout>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QColorDialog>
#include <QtMath>
#include <sourcepp/String.h>
#include <vtfpp/vtfpp.h>

#include "../../../utility/ThemedIcon.h"
#include "../../../utility/Options.h"

using namespace kvpp;
using namespace mdlpp;
using namespace sourcepp;
using namespace std::literals;
using namespace vtfpp;

MDLPreview::~MDLPreview() = default;

namespace {

struct EmbeddedModelBuffers {
	const std::byte* vtxData = nullptr;
	std::size_t vtxSize = 0;
	const std::byte* vvdData = nullptr;
	std::size_t vvdSize = 0;
};

static bool readI32LE(const std::byte* data, std::size_t size, std::size_t offset, std::int32_t& out) {
	if (!data || offset + sizeof(out) > size) {
		return false;
	}
	std::memcpy(&out, data + offset, sizeof(out));
	return true;
}

static EmbeddedModelBuffers findEmbeddedModelBuffers(const std::byte* mdlData, std::size_t mdlSize, const MDL::MDL& mdl) {
	EmbeddedModelBuffers out{};
	if (!mdlData || mdlSize < 64) {
		return out;
	}

	// Titanfall 2 use models as a single .mdl that embeds the usual sidecar data
	// Respawn MDL headers can also include explicit embedded offsets/sizes;
	// we dont hardcode struct layouts here, but we can detect the offset+size block by validating invariants

	// Try to locate an embedded-loose-data block (vtxOffset/vvdOffset/vvcOffset/phyOffset + sizes) in the header
	// The block is 8 int32 values; if present it should satisfy:
	// - each (offset,size) either both >0 and within file bounds, or size==0/offset==0/-1 (unused)
	// - VVD must start with "IDSV" and validate against the MDL checksum
	// - VTX must be version 7 and validate against the MDL checksum
	{
		const auto scanLimit = std::min<std::size_t>(mdlSize, 0x400); // header-ish
		for (std::size_t i = 0; i + 8 * sizeof(std::int32_t) <= scanLimit; i += 4) {
			std::int32_t vtxOff = 0, vvdOff = 0, vvcOff = 0, phyOff = 0;
			std::int32_t vtxSize = 0, vvdSize = 0, vvcSize = 0, phySize = 0;
			if (!readI32LE(mdlData, mdlSize, i + 0, vtxOff) ||
				!readI32LE(mdlData, mdlSize, i + 4, vvdOff) ||
				!readI32LE(mdlData, mdlSize, i + 8, vvcOff) ||
				!readI32LE(mdlData, mdlSize, i + 12, phyOff) ||
				!readI32LE(mdlData, mdlSize, i + 16, vtxSize) ||
				!readI32LE(mdlData, mdlSize, i + 20, vvdSize) ||
				!readI32LE(mdlData, mdlSize, i + 24, vvcSize) ||
				!readI32LE(mdlData, mdlSize, i + 28, phySize)) {
				continue;
			}

			auto validPair = [&](std::int32_t off, std::int32_t sz) -> bool {
				if (sz <= 0) {
					return true;
				}
				if (off <= 0) {
					return false;
				}
				const auto uoff = static_cast<std::size_t>(off);
				const auto usz = static_cast<std::size_t>(sz);
				return uoff < mdlSize && uoff + usz <= mdlSize;
			};

			if (!validPair(vtxOff, vtxSize) || !validPair(vvdOff, vvdSize) || !validPair(vvcOff, vvcSize) || !validPair(phyOff, phySize)) {
				continue;
			}

			bool ok = false;
			if (vvdSize > 0) {
				const auto uoff = static_cast<std::size_t>(vvdOff);
				const auto usz = static_cast<std::size_t>(vvdSize);
				VVD::VVD vvd;
				if (vvd.open(mdlData + uoff, usz, mdl)) {
					out.vvdData = mdlData + uoff;
					out.vvdSize = usz;
					ok = true;
				}
			}

			if (vtxSize > 0) {
				const auto uoff = static_cast<std::size_t>(vtxOff);
				const auto usz = static_cast<std::size_t>(vtxSize);
				VTX::VTX vtx;
				if (vtx.open(mdlData + uoff, usz, mdl)) {
					out.vtxData = mdlData + uoff;
					out.vtxSize = usz;
					ok = true;
				}
			}

			if (ok) {
				return out;
			}
		}
	}

	// If we didn't find an explicit embedded block, fall back to scanning the .mdl bytes and validating against the MDL checksum

	// VVD starts with FourCC "IDSV" and has a checksum field that must match the MDL checksum
	static constexpr std::array<std::byte, 4> kIDSV{
		static_cast<std::byte>('I'),
		static_cast<std::byte>('D'),
		static_cast<std::byte>('S'),
		static_cast<std::byte>('V'),
	};
	for (std::size_t i = 0; i + kIDSV.size() <= mdlSize; i++) {
		if (std::memcmp(mdlData + i, kIDSV.data(), kIDSV.size()) != 0) {
			continue;
		}
		const auto* candidate = mdlData + i;
		const auto candidateSize = mdlSize - i;
		if (candidateSize < 64) {
			continue;
		}
		VVD::VVD vvd;
		if (vvd.open(candidate, candidateSize, mdl)) {
			out.vvdData = candidate;
			out.vvdSize = candidateSize;
			break;
		}
	}

	// VTX starts with int32 version=7 and includes the MDL checksum at offset +20
	// Scan for plausible VTX headers and validate by attempting to parse. Some branches (Respawn) shift the checksum by 4 bytes
	for (std::size_t start = 0; start + 24 <= mdlSize; start++) {
		std::int32_t ver = 0;
		if (!readI32LE(mdlData, mdlSize, start, ver) || ver != 7) {
			continue;
		}
		std::uint32_t checkSumStd = 0;
		std::memcpy(&checkSumStd, mdlData + start + 20, sizeof(checkSumStd));
		std::uint32_t checkSumShort = 0;
		std::memcpy(&checkSumShort, mdlData + start + 16, sizeof(checkSumShort));
		if (checkSumStd != mdl.checksum && checkSumShort != mdl.checksum) {
			continue;
		}

		const auto candidateSize = mdlSize - start;
		if (candidateSize < 44) {
			continue;
		}

		VTX::VTX vtx;
		if (vtx.open(mdlData + start, candidateSize, mdl)) {
			out.vtxData = mdlData + start;
			out.vtxSize = candidateSize;
			break;
		}
	}

	return out;
}

std::unique_ptr<MDLTextureData> getTextureDataForMaterial(IVPKEditWindowAccess_V3* windowAccess, const std::string& materialPath) {
	QString materialFile;
	if (!windowAccess->readTextEntry(materialPath.c_str(), materialFile)) {
		return nullptr;
	}

	const KV1 materialKV{materialFile.toUtf8().constData()};
	if (materialKV.getChildCount() == 0) {
		return nullptr;
	}

	std::string baseTexturePath;
	if (const auto& baseTexturePathKV = materialKV[0]["$basetexture"]; !baseTexturePathKV.isInvalid()) {
		baseTexturePath = baseTexturePathKV.getValue();
	} else if (string::iequals(materialKV[0].getKey(), "patch")) {
		if (const auto& baseTexturePathPatchInsertKV = materialKV[0]["insert"]["$basetexture"]; !baseTexturePathPatchInsertKV.isInvalid()) {
			baseTexturePath = baseTexturePathPatchInsertKV.getValue();
		} else if (const auto& baseTexturePathPatchReplaceKV = materialKV[0]["replace"]["$basetexture"]; !baseTexturePathPatchReplaceKV.isInvalid()) {
			baseTexturePath = baseTexturePathPatchReplaceKV.getValue();
		} else if (const auto& baseTexturePathPatchIncludeKV = materialKV[0]["include"]; !baseTexturePathPatchIncludeKV.isInvalid()) {
			// Just re-using this variable for the new material path
			baseTexturePath = baseTexturePathPatchIncludeKV.getValue();
			string::normalizeSlashes(baseTexturePath);
			return ::getTextureDataForMaterial(windowAccess, baseTexturePath);
		} else {
			return nullptr;
		}
	} else {
		return nullptr;
	}

	QByteArray textureFile;
	if (!windowAccess->readBinaryEntry(("materials/" + baseTexturePath + ".vtf").c_str(), textureFile)) {
		return nullptr;
	}

	// todo: properly handle patch materials
	bool translucent = !materialKV[0]["$translucent"].isInvalid() && materialKV[0]["$translucent"].getValue<bool>();
	bool alphaTest = false;
	float alphaTestReference = 0.f;
	if (!translucent) {
		alphaTest = !materialKV[0]["$alphatest"].isInvalid() && materialKV[0]["$alphatest"].getValue<bool>();
		if (alphaTest && !materialKV[0]["$alphatestreference"].isInvalid()) {
			alphaTestReference = materialKV[0]["$alphatestreference"].getValue<float>();
		} else if (alphaTest) {
			alphaTestReference = 0.7f;
		}
	}

	const VTF vtf{{reinterpret_cast<std::byte*>(textureFile.data()), static_cast<std::span<const std::byte>::size_type>(textureFile.size())}};
	if (!ImageFormatDetails::transparent(vtf.getFormat())) {
		translucent = false;
		alphaTest = false;
		alphaTestReference = 0.f;
	}

	return std::make_unique<MDLTextureData>(
		(translucent || alphaTest) ? vtf.getImageDataAsRGBA8888() : vtf.getImageDataAs(ImageFormat::RGB888),
		vtf.getWidth(),
		vtf.getHeight(),
		MDLTextureSettings{
			translucent ? MDLTextureSettings::TransparencyMode::TRANSLUCENT : alphaTest ? MDLTextureSettings::TransparencyMode::ALPHA_TEST : MDLTextureSettings::TransparencyMode::NONE,
			alphaTestReference,
		}
	);
}

} // namespace

QList<QVector3D> AABB::getCorners() const {
	return {
		{this->min.x(), this->min.y(), this->min.z()},
		{this->max.x(), this->min.y(), this->min.z()},
		{this->min.x(), this->max.y(), this->min.z()},
		{this->min.x(), this->min.y(), this->max.z()},
		{this->max.x(), this->max.y(), this->max.z()},
		{this->min.x(), this->max.y(), this->max.z()},
		{this->max.x(), this->min.y(), this->max.z()},
		{this->max.x(), this->max.y(), this->min.z()},
	};
}

float AABB::getWidth() const {
	return this->max.x() - this->min.x();
}

float AABB::getHeight() const {
	return this->max.y() - this->min.y();
}

float AABB::getDepth() const {
	return this->max.z() - this->min.z();
}

MDLWidget::MDLWidget(QWidget* parent)
		: QOpenGLWidget(parent)
		, missingTexture(QOpenGLTexture::Target2D)
		, matCapTexture(QOpenGLTexture::Target2D)
		, vertexCount(0)
		, skin(0)
		, shadingMode(MDLShadingMode::UNSHADED_TEXTURED)
		, distance(0.0)
		, distanceScale(0.0)
		, fov(70.0)
		, cullBackFaces(true)
		, angularSpeed(0.0)
		, rmbBeingHeld(false) {}

MDLWidget::~MDLWidget() {
	this->clearMeshes();

	// Destroy grid GL resources.
	if (this->gridVao.isCreated()) {
		this->gridVao.destroy();
	}
	if (this->gridVertices.isCreated()) {
		this->gridVertices.destroy();
	}

	if (this->missingTexture.isCreated()) {
		this->missingTexture.destroy();
	}
	if (this->matCapTexture.isCreated()) {
		this->matCapTexture.destroy();
	}
}

void MDLWidget::setModel(const BakedModel& model) {
	this->makeCurrent();

	// Clear previous geometry but keep textures/skins (bodygroup toggles rebuild the mesh frequently).
	for (auto& mesh : this->meshes) {
		if (mesh.vao) {
			mesh.vao->destroy();
			mesh.vao.reset();
		}
		if (mesh.ebo.isCreated()) {
			mesh.ebo.destroy();
		}
	}
	this->meshes.clear();

	// Set vertex data
	if (this->vertices.isCreated()) {
		this->vertices.destroy();
	}
	this->vertexCount = static_cast<int>(model.vertices.size());
	this->vertices.create();
	this->vertices.bind();
	this->vertices.setUsagePattern(QOpenGLBuffer::StaticDraw);
	this->vertices.allocate(model.vertices.data(), static_cast<int>(this->vertexCount * sizeof(BakedModel::Vertex)));
	this->vertices.release();

	// Add meshes
	for (const auto& bakedMesh : model.meshes) {
		auto& mesh = this->meshes.emplace_back();
		mesh.vao = std::make_unique<QOpenGLVertexArrayObject>();
		mesh.vao->create();
		mesh.vao->bind();

		this->vertices.bind();

		std::ptrdiff_t offset = 0;
		// position
		this->glEnableVertexAttribArray(0);
		this->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BakedModel::Vertex), reinterpret_cast<void*>(offset));
		offset += sizeof(math::Vec3f);

		// normal
		this->glEnableVertexAttribArray(1);
		this->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(BakedModel::Vertex), reinterpret_cast<void*>(offset));
		offset += sizeof(math::Vec3f);

		// uv
		this->glEnableVertexAttribArray(2);
		this->glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(BakedModel::Vertex), reinterpret_cast<void*>(offset));
		// offset += sizeof(math::Vec2f);

		mesh.textureIndex = bakedMesh.materialIndex;

		mesh.indexCount = static_cast<int>(bakedMesh.indices.size());
		mesh.ebo.create();
		mesh.ebo.bind();
		mesh.ebo.allocate(bakedMesh.indices.data(), static_cast<int>(mesh.indexCount * sizeof(uint16_t)));

		mesh.vao->release();

		mesh.ebo.release();

		this->vertices.release();
	}
}

void MDLWidget::setTextures(const std::vector<std::unique_ptr<MDLTextureData>>& vtfData) {
	this->makeCurrent();

	this->clearTextures();
	for (const auto& vtf : vtfData) {
		if (!vtf) {
			this->textures.push_back({nullptr, {}});
			continue;
		}
		auto* texture = new QOpenGLTexture(QOpenGLTexture::Target::Target2D);
		texture->create();
		texture->setData(QImage(reinterpret_cast<uchar*>(vtf->data.data()), vtf->width, vtf->height, vtf->settings.transparencyMode == MDLTextureSettings::TransparencyMode::NONE ? QImage::Format_RGB888 : QImage::Format_RGBA8888));
		this->textures.push_back({texture, vtf->settings});
	}
}

void MDLWidget::clearTextures() {
	this->makeCurrent();

	for (auto* texture : this->textures | std::views::keys) {
		if (texture && texture->isCreated()) {
			texture->destroy();
		}
		delete texture;
	}
	this->textures.clear();
}

void MDLWidget::setSkinLookupTable(std::vector<std::vector<short>> skins_) {
	this->skins = std::move(skins_);
}

void MDLWidget::setAABB(AABB aabb) {
	// https://stackoverflow.com/a/32836605 - calculate optimal camera distance from bounding box
	const auto midpoint = (aabb.max + aabb.min) / 2.0f;
	float sphereRadius = 0.0f;
	for (const auto corner : aabb.getCorners()) {
		if (const auto dist = midpoint.distanceToPoint(corner); dist > sphereRadius) {
			sphereRadius = dist;
		}
	}
	const float fovRad = qDegreesToRadians(this->fov);
	this->target = midpoint;
	this->distance = sphereRadius / qTan(fovRad / 2);
	this->distanceScale = this->distance / 128.0f;
}

void MDLWidget::setSkin(int skin_) {
	this->skin = skin_;
	this->update();
}

void MDLWidget::setShadingMode(MDLShadingMode type) {
	this->shadingMode = type;
	this->update();
}

void MDLWidget::setFieldOfView(float newFOV) {
	this->fov = newFOV;
	this->update();
}

void MDLWidget::setCullBackFaces(bool enable) {
	this->cullBackFaces = enable;
	this->update();
}

void MDLWidget::clearMeshes() {
	this->makeCurrent();

	for (auto& mesh : this->meshes) {
		if (mesh.vao) {
			mesh.vao->destroy();
			mesh.vao.reset();
		}
		if (mesh.ebo.isCreated()) {
			mesh.ebo.destroy();
		}
	}
	this->meshes.clear();

	if (this->vertices.isCreated()) {
		this->vertices.destroy();
	}

	this->clearTextures();

	this->skin = 0;
	this->skins.clear();
}

void MDLWidget::initializeGL() {
	if (!this->initializeOpenGLFunctions()) {
		QMessageBox::critical(this, tr("Error"), tr("Unable to initialize OpenGL 3.3 Core context! Please upgrade your computer to preview models."));
		return; // and probably crash right after
	}

	// Grid shader (simple vertex-colored lines)
	this->gridShaderProgram.addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shaders/grid.vert");
	this->gridShaderProgram.addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shaders/grid.frag");
	this->gridShaderProgram.link();

	this->gridVertices.create();
	this->gridVertices.setUsagePattern(QOpenGLBuffer::DynamicDraw);

	this->gridVao.create();
	this->gridVao.bind();
	this->gridVertices.bind();
	this->glEnableVertexAttribArray(0);
	this->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 7, nullptr);
	this->glEnableVertexAttribArray(1);
	this->glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(float) * 7, reinterpret_cast<void*>(sizeof(float) * 3));
	this->gridVertices.release();
	this->gridVao.release();

	this->wireframeShaderProgram.addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shaders/mdl.vert");
	this->wireframeShaderProgram.addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shaders/mdl_wireframe.frag");
	this->wireframeShaderProgram.link();

	this->shadedUntexturedShaderProgram.addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shaders/mdl.vert");
	this->shadedUntexturedShaderProgram.addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shaders/mdl_shaded_untextured.frag");
	this->shadedUntexturedShaderProgram.link();

	this->unshadedTexturedShaderProgram.addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shaders/mdl.vert");
	this->unshadedTexturedShaderProgram.addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shaders/mdl_unshaded_textured.frag");
	this->unshadedTexturedShaderProgram.link();

	this->shadedTexturedShaderProgram.addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shaders/mdl.vert");
	this->shadedTexturedShaderProgram.addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shaders/mdl_shaded_textured.frag");
	this->shadedTexturedShaderProgram.link();

	this->missingTexture.create();
	this->missingTexture.setData(QImage(":/textures/checkerboard.png"));

	this->matCapTexture.create();
	this->matCapTexture.setData(QImage(":/textures/default_matcap.png"));

	this->timer.start(12, this);

	// Build initial grid after GL init so buffers exist.
	this->rebuildGridGeometry();
}

void MDLWidget::resizeGL(int w, int h) {
	this->glViewport(0, 0, w, h);

	const float aspectRatio = static_cast<float>(w) / static_cast<float>(h > 0 ? h : 1);
	const float nearPlane = 0.015f, farPlane = 32768.0f;
	this->projection.setToIdentity();
	this->projection.perspective(this->fov, aspectRatio, nearPlane, farPlane);
}

void MDLWidget::paintGL() {
	QStyleOption opt;
	opt.initFrom(this);

	const auto clearColor = opt.palette.color(QPalette::ColorRole::Window);
	this->glClearColor(clearColor.redF(), clearColor.greenF(), clearColor.blueF(), 1.f);
	this->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	this->glEnable(GL_MULTISAMPLE);
	this->glEnable(GL_DEPTH_TEST);

	// Orbit camera:
	// - `rotation` is the camera orientation (roll-free turntable)
	// - `target` is the point we're orbiting around
	// - `distance` is the camera radius
	//
	// Build a proper view matrix so orbit/pan feel like Blender (screen-relative) and never "drift" into model axes.
	const QVector3D forward = this->rotation.rotatedVector(QVector3D{0.0f, 0.0f, -1.0f});
	const QVector3D up = this->rotation.rotatedVector(QVector3D{0.0f, 1.0f, 0.0f});
	// Avoid degenerate lookAt (eye==target) when no model is loaded / distance is ~0.
	const float safeDistance = std::max(0.001f, this->distance);
	const QVector3D eye = this->target - (forward * safeDistance);
	QMatrix4x4 view;
	view.lookAt(eye, this->target, up);

	// Draw grid even if there's no model loaded yet.
	if (this->gridEnabled && this->gridVertexCount > 0) {
		this->glDisable(GL_CULL_FACE);
		this->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		this->glEnable(GL_BLEND);
		this->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		this->gridShaderProgram.bind();
		this->gridShaderProgram.setUniformValue("uMVP", this->projection * view);

		this->gridVao.bind();
		this->glDrawArrays(GL_LINES, 0, this->gridVertexCount);
		this->gridVao.release();
		this->gridShaderProgram.release();

		this->glDisable(GL_BLEND);
	}

	if (this->meshes.empty()) {
		return;
	}

	if (!this->cullBackFaces || this->shadingMode == MDLShadingMode::WIREFRAME) {
		this->glDisable(GL_CULL_FACE);
	} else {
		this->glEnable(GL_CULL_FACE);
	}

	if (this->shadingMode == MDLShadingMode::WIREFRAME) {
		this->glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	} else {
		this->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}

	QOpenGLShaderProgram* currentShaderProgram = nullptr;
	switch (this->shadingMode) {
		case MDLShadingMode::WIREFRAME:
			currentShaderProgram = &this->wireframeShaderProgram;
			break;
		case MDLShadingMode::SHADED_UNTEXTURED:
			currentShaderProgram = &this->shadedUntexturedShaderProgram;
			break;
		case MDLShadingMode::UNSHADED_TEXTURED:
			currentShaderProgram = &this->unshadedTexturedShaderProgram;
			break;
		case MDLShadingMode::SHADED_TEXTURED:
			currentShaderProgram = &this->shadedTexturedShaderProgram;
			break;
	}
	if (!currentShaderProgram) {
		return;
	}
	currentShaderProgram->bind();

	currentShaderProgram->setUniformValue("uMVP", this->projection * view);
	currentShaderProgram->setUniformValue("uMV", view);
	currentShaderProgram->setUniformValue("uNormalMatrix", view.normalMatrix());
	currentShaderProgram->setUniformValue("uEyePosition", eye);
	currentShaderProgram->setUniformValue("uMeshTexture", 0);
	currentShaderProgram->setUniformValue("uMatCapTexture", 1);

	QList<QPair<MDLSubMesh*, QPair<QOpenGLTexture*, MDLTextureSettings>>> opaqueMeshes;
	QList<QPair<MDLSubMesh*, QPair<QOpenGLTexture*, MDLTextureSettings>>> alphaTestMeshes;
	QList<QPair<MDLSubMesh*, QPair<QOpenGLTexture*, MDLTextureSettings>>> translucentMeshes;
	for (auto& mesh : this->meshes) {
		QOpenGLTexture* texture;
		if (mesh.textureIndex < 0 || this->skins.size() <= this->skin || this->skins[this->skin].size() <= mesh.textureIndex || !((texture = this->textures[this->skins[this->skin][mesh.textureIndex]].first))) {
			texture = &this->missingTexture;
			opaqueMeshes.push_back({&mesh, {texture, {}}});
			continue;
		}
		switch (const auto& settings = this->textures[this->skins[this->skin][mesh.textureIndex]].second; settings.transparencyMode) {
			case MDLTextureSettings::TransparencyMode::NONE:
				opaqueMeshes.push_back({&mesh, {texture, settings}});
				break;
			case MDLTextureSettings::TransparencyMode::ALPHA_TEST:
				alphaTestMeshes.push_back({&mesh, {texture, settings}});
				break;
			case MDLTextureSettings::TransparencyMode::TRANSLUCENT:
				translucentMeshes.push_back({&mesh, {texture, settings}});
				break;
		}
	}
	for (const auto& currentMeshes : {opaqueMeshes, alphaTestMeshes, translucentMeshes}) {
		for (const auto& [subMesh, textureData] : currentMeshes) {
			currentShaderProgram->setUniformValue("uAlphaTestReference", textureData.second.alphaTestReference);

			if (this->shadingMode != MDLShadingMode::WIREFRAME && textureData.second.transparencyMode == MDLTextureSettings::TransparencyMode::TRANSLUCENT) {
				this->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				this->glEnable(GL_BLEND);
			} else {
				this->glDisable(GL_BLEND);
			}

			this->glActiveTexture(GL_TEXTURE0);
			textureData.first->bind();
			this->glActiveTexture(GL_TEXTURE1);
			this->matCapTexture.bind();

			subMesh->vao->bind();
			this->glDrawElements(GL_TRIANGLES, subMesh->indexCount, GL_UNSIGNED_SHORT, nullptr);
			subMesh->vao->release();

			this->glActiveTexture(GL_TEXTURE1);
			this->matCapTexture.release();
			this->glActiveTexture(GL_TEXTURE0);
			textureData.first->release();
		}
	}

	currentShaderProgram->release();
}

void MDLWidget::mousePressEvent(QMouseEvent* event) {
	this->mousePressPosition = QVector2D(event->position());

	const auto mods = QApplication::queryKeyboardModifiers();
	const bool altLmb = (event->button() == Qt::MouseButton::LeftButton) && (mods & Qt::KeyboardModifier::AltModifier);
	const bool orbitButton = (event->button() == Qt::MouseButton::MiddleButton) || (event->button() == Qt::MouseButton::RightButton) || altLmb;

	// Blender-ish navigation:
	// - Orbit: MMB/RMB drag (or Alt+LMB)
	// - Pan: Shift + Orbit gesture
	// - Dolly zoom: Ctrl + Orbit gesture
	if (orbitButton) {
		if (mods & Qt::KeyboardModifier::ShiftModifier) {
			this->interactionMode = InteractionMode::PAN;
		} else if (mods & Qt::KeyboardModifier::ControlModifier) {
			this->interactionMode = InteractionMode::DOLLY;
		} else {
			this->interactionMode = InteractionMode::ORBIT;

			// Strip any accumulated roll when starting an orbit gesture by converting the
			// current orientation into turntable yaw/pitch and reconstructing rotation.
			// This keeps Blender-like orbit stable and prevents the camera from becoming rolled.
			const QVector3D fwd = this->rotation.rotatedVector(QVector3D{0.0f, 0.0f, -1.0f});
			const float fy = std::clamp(fwd.y(), -1.0f, 1.0f);
			this->orbitYawDeg = -qRadiansToDegrees(std::atan2(fwd.x(), -fwd.z()));
			this->orbitPitchDeg = qRadiansToDegrees(std::asin(fy));
			this->orbitPitchDeg = std::clamp(this->orbitPitchDeg, -89.9f, 89.9f);

			const QQuaternion yaw = QQuaternion::fromAxisAndAngle(QVector3D{0.0f, 1.0f, 0.0f}, this->orbitYawDeg);
			const QVector3D rightAxis = yaw.rotatedVector(QVector3D{1.0f, 0.0f, 0.0f});
			const QQuaternion pitch = QQuaternion::fromAxisAndAngle(rightAxis, this->orbitPitchDeg);
			this->rotation = (pitch * yaw).normalized();
		}
	} else {
		this->interactionMode = InteractionMode::NONE;
	}

	if (event->button() == Qt::MouseButton::RightButton) {
		this->rmbBeingHeld = true;
	}

	// Kill any inertial rotation so the model doesn't "drift" after interactions.
	this->angularSpeed = 0.0;
	if (this->interactionMode != InteractionMode::NONE) {
		this->setCursor({Qt::CursorShape::ClosedHandCursor});
	}
	event->accept();
}

void MDLWidget::mouseReleaseEvent(QMouseEvent* event) {
	if (event->buttons() == Qt::NoButton) {
		this->setCursor({Qt::CursorShape::ArrowCursor});
		this->interactionMode = InteractionMode::NONE;
	}
	if (event->button() == Qt::MouseButton::RightButton) {
		this->rmbBeingHeld = false;
	}
	// Ensure we never keep rotating after releasing the mouse.
	this->angularSpeed = 0.0;
	event->accept();
}

void MDLWidget::mouseMoveEvent(QMouseEvent* event) {
	// Only react to our navigation gestures (Blender-ish). Otherwise ignore.
	// Note: We track the mode on press; this prevents "accidental" orbit while selecting.
	if (this->interactionMode == InteractionMode::NONE) {
		return;
	}

	const QVector2D currentPos{event->position()};
	const QVector2D diff = currentPos - this->mousePressPosition;
	this->mousePressPosition = currentPos;

	// Pan (view-relative)
	if (this->interactionMode == InteractionMode::PAN) {
		const QVector3D rightAxis = this->rotation.rotatedVector(QVector3D{1.0f, 0.0f, 0.0f});
		const QVector3D upAxis = this->rotation.rotatedVector(QVector3D{0.0f, 1.0f, 0.0f});

		// Match Blender feel: drag right moves scene right; drag up moves scene up.
		const float kPanScale = this->distanceScale / 4.0f;
		this->translationalVelocity = (rightAxis * (diff.x() * kPanScale)) + (upAxis * (-diff.y() * kPanScale));
		this->target += this->translationalVelocity;
		this->update();
		event->accept();
		return;
	}

	// Dolly (Ctrl+orbit gesture): drag up/down changes distance.
	if (this->interactionMode == InteractionMode::DOLLY) {
		this->distance -= static_cast<float>(diff.y()) * this->distanceScale;
		this->angularSpeed = 0.0;
		this->update();
		event->accept();
		return;
	}

	// Orbit (turntable-like): yaw around world up (+Y), pitch around yaw-relative right axis.
	// Lock roll by driving rotation from yaw/pitch angles (Blender "Turntable" orbit style).
	{
		constexpr float kSensitivityDegPerPixel = 0.25f;
		// Invert left/right orbit direction.
		this->orbitYawDeg -= diff.x() * kSensitivityDegPerPixel;
		// Invert up/down: dragging up should pitch up.
		this->orbitPitchDeg = std::clamp(this->orbitPitchDeg - (diff.y() * kSensitivityDegPerPixel), -89.9f, 89.9f);

		const QQuaternion yaw = QQuaternion::fromAxisAndAngle(QVector3D{0.0f, 1.0f, 0.0f}, this->orbitYawDeg);
		const QVector3D rightAxis = yaw.rotatedVector(QVector3D{1.0f, 0.0f, 0.0f});
		const QQuaternion pitch = QQuaternion::fromAxisAndAngle(rightAxis, this->orbitPitchDeg);

		this->rotation = (pitch * yaw).normalized();
	}
	this->angularSpeed = 0.0;
	this->update();

	event->accept();
}

void MDLWidget::wheelEvent(QWheelEvent* event) {
	if (QPoint numDegrees = event->angleDelta() / 8; !numDegrees.isNull()) {
		this->distance -= static_cast<float>(numDegrees.y()) * this->distanceScale;
		this->update();
	}
	event->accept();
}

void MDLWidget::setGridEnabled(bool enable) {
	if (this->gridEnabled == enable) {
		return;
	}
	this->gridEnabled = enable;
	this->update();
}

void MDLWidget::setGridSpacing(float spacing) {
	spacing = std::max(0.001f, spacing);
	if (std::abs(this->gridSpacing - spacing) < 0.0001f) {
		return;
	}
	this->gridSpacing = spacing;
	this->rebuildGridGeometry();
	this->update();
}

void MDLWidget::setGridExtentCells(int extentCells) {
	extentCells = std::max(1, extentCells);
	if (this->gridExtentCells == extentCells) {
		return;
	}
	this->gridExtentCells = extentCells;
	this->rebuildGridGeometry();
	this->update();
}

void MDLWidget::setGridMajorEvery(int majorEvery) {
	majorEvery = std::max(1, majorEvery);
	if (this->gridMajorEvery == majorEvery) {
		return;
	}
	this->gridMajorEvery = majorEvery;
	this->rebuildGridGeometry();
	this->update();
}

void MDLWidget::setGridColors(const QColor& minorColor, const QColor& majorColor) {
	if (this->gridMinorColor == minorColor && this->gridMajorColor == majorColor) {
		return;
	}
	this->gridMinorColor = minorColor;
	this->gridMajorColor = majorColor;
	this->rebuildGridGeometry();
	this->update();
}

void MDLWidget::rebuildGridGeometry() {
	const int extent = std::max(1, this->gridExtentCells);
	const int majorEvery = std::max(1, this->gridMajorEvery);
	const float spacing = std::max(0.001f, this->gridSpacing);

	const auto toRGBA = [](const QColor& c) -> std::array<float, 4> {
		return {c.redF(), c.greenF(), c.blueF(), c.alphaF()};
	};
	const auto minor = toRGBA(this->gridMinorColor);
	const auto major = toRGBA(this->gridMajorColor);

	std::vector<float> v;
	v.reserve(static_cast<size_t>((extent * 2 + 1) * 4) * 7);

	const float minCoord = -static_cast<float>(extent) * spacing;
	const float maxCoord = static_cast<float>(extent) * spacing;

	auto emitVertex = [&v](float x, float y, float z, const std::array<float, 4>& c) {
		v.push_back(x);
		v.push_back(y);
		v.push_back(z);
		v.push_back(c[0]);
		v.push_back(c[1]);
		v.push_back(c[2]);
		v.push_back(c[3]);
	};

	for (int i = -extent; i <= extent; i++) {
		const bool isMajor = (i % majorEvery) == 0;
		const auto& c = isMajor ? major : minor;
		const float coord = static_cast<float>(i) * spacing;

		emitVertex(coord, 0.0f, minCoord, c);
		emitVertex(coord, 0.0f, maxCoord, c);

		emitVertex(minCoord, 0.0f, coord, c);
		emitVertex(maxCoord, 0.0f, coord, c);
	}

	this->gridVertexCount = static_cast<int>(v.size() / 7);

	if (this->context() && this->context()->isValid()) {
		this->makeCurrent();
		this->gridVertices.bind();
		this->gridVertices.allocate(v.data(), static_cast<int>(v.size() * sizeof(float)));
		this->gridVertices.release();
		this->doneCurrent();
	}
}

constexpr float MOTION_REDUCTION_AMOUNT = 0.75f;

void MDLWidget::timerEvent(QTimerEvent* /*event*/) {
	this->translationalVelocity *= MOTION_REDUCTION_AMOUNT;
	if (this->translationalVelocity.length() < 0.01) {
		this->translationalVelocity = QVector3D();
		this->update();
	} else {
		this->target += this->translationalVelocity;
		this->update();
	}
}

constexpr int TOOLBAR_SPACE_SIZE = 48;
constexpr int SHADING_MODE_BUTTON_SIZE = 24;

void MDLPreview::initPlugin(IVPKEditWindowAccess_V3* windowAccess_) {
	this->windowAccess = windowAccess_;
}

void MDLPreview::initPreview(QWidget* parent) {
	this->preview = new QWidget{parent};

	auto* layout = new QVBoxLayout(this->preview);
	layout->setContentsMargins(0,0,0,0);

	auto* controls = new QFrame(this->preview);
	controls->setFrameShape(QFrame::Shape::StyledPanel);
	controls->setFixedHeight(TOOLBAR_SPACE_SIZE);
	layout->addWidget(controls, Qt::AlignRight);

	auto* controlsLayout = new QHBoxLayout(controls);
	controlsLayout->setAlignment(Qt::AlignRight);

	// Grid controls (moved to the left side of the toolbar, swapping position with the info panel toggle).
	auto* gridToggle = new QCheckBox(tr("Grid"), controls);
	gridToggle->setChecked(Options::get<bool>(OPT_MDL_GRID_ENABLED));
	controlsLayout->addWidget(gridToggle);

	auto* gridSettings = new QToolButton(controls);
	gridSettings->setText(tr("Edit Grid"));
	gridSettings->setPopupMode(QToolButton::InstantPopup);
	controlsLayout->addWidget(gridSettings);

	auto* gridMenu = new QMenu(gridSettings);
	gridSettings->setMenu(gridMenu);

	auto* panel = new QWidget(gridMenu);
	auto* form = new QFormLayout(panel);
	form->setContentsMargins(8, 8, 8, 8);
	form->setRowWrapPolicy(QFormLayout::DontWrapRows);

	auto* spacingSpin = new QDoubleSpinBox(panel);
	spacingSpin->setDecimals(3);
	spacingSpin->setRange(0.001, 4096.0);
	spacingSpin->setValue(Options::get<double>(OPT_MDL_GRID_SPACING));
	form->addRow(tr("Box Size"), spacingSpin);

	auto* extentSpin = new QSpinBox(panel);
	extentSpin->setRange(1, 500);
	extentSpin->setValue(Options::get<int>(OPT_MDL_GRID_EXTENT_CELLS));
	form->addRow(tr("Extent (Cells)"), extentSpin);

	auto* majorEverySpin = new QSpinBox(panel);
	majorEverySpin->setRange(1, 100);
	majorEverySpin->setValue(Options::get<int>(OPT_MDL_GRID_MAJOR_EVERY));
	form->addRow(tr("Major Every"), majorEverySpin);

	auto* minorColorBtn = new QPushButton(tr("Minor..."), panel);
	auto* majorColorBtn = new QPushButton(tr("Major..."), panel);
	form->addRow(tr("Colors"), minorColorBtn);
	form->addRow(QString{}, majorColorBtn);

	auto syncColorButton = [](QPushButton* btn, const QColor& c) {
		btn->setStyleSheet(QString("QPushButton { background-color: rgba(%1,%2,%3,%4); border: 1px solid rgba(255,255,255,40); }")
			.arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha()));
	};
	syncColorButton(minorColorBtn, Options::get<QColor>(OPT_MDL_GRID_MINOR_COLOR));
	syncColorButton(majorColorBtn, Options::get<QColor>(OPT_MDL_GRID_MAJOR_COLOR));

	auto* action = new QWidgetAction(gridMenu);
	action->setDefaultWidget(panel);
	gridMenu->addAction(action);

	controlsLayout->addSpacing(TOOLBAR_SPACE_SIZE);

	this->backfaceCulling = new QCheckBox(tr("Backface Culling"), controls);
	this->backfaceCulling->setCheckState(Qt::CheckState::Checked);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	QObject::connect(this->backfaceCulling, &QCheckBox::checkStateChanged, this, [&](Qt::CheckState state) {
#else
	QObject::connect(this->backfaceCulling, &QCheckBox::stateChanged, this, [&](int state) {
#endif
		this->mdl->setCullBackFaces(state == Qt::CheckState::Checked);
	});
	controlsLayout->addWidget(this->backfaceCulling);

	controlsLayout->addSpacing(TOOLBAR_SPACE_SIZE);

	controlsLayout->addWidget(new QLabel(tr("Skin"), controls));
	this->skinSpinBox = new QSpinBox(controls);
	this->skinSpinBox->setFixedWidth(32);
	this->skinSpinBox->setMinimum(0);
	this->skinSpinBox->setValue(0);
	QObject::connect(this->skinSpinBox, &QSpinBox::valueChanged, this, [&](int value) {
		this->mdl->setSkin(value);
	});
	controlsLayout->addWidget(this->skinSpinBox);

	controlsLayout->addSpacing(TOOLBAR_SPACE_SIZE);

	const QList<QPair<QToolButton**, Qt::Key>> buttons{
		{&this->shadingModeWireframe,        Qt::Key_1},
		{&this->shadingModeShadedUntextured, Qt::Key_2},
		{&this->shadingModeUnshadedTextured, Qt::Key_3},
		{&this->shadingModeShadedTextured,   Qt::Key_4},
	};
	for (int i = 0; i < buttons.size(); i++) {
		auto* button = *(buttons[i].first) = new QToolButton(controls);
		button->setToolButtonStyle(Qt::ToolButtonIconOnly);
		button->setFixedSize(SHADING_MODE_BUTTON_SIZE, SHADING_MODE_BUTTON_SIZE);
		button->setStyleSheet(
				"QToolButton         { background-color: rgba(0,0,0,0); border: none; }\n"
				"QToolButton:pressed { background-color: rgba(0,0,0,0); border: none; }");
		button->setShortcut(buttons[i].second);
		QObject::connect(button, &QToolButton::pressed, this, [this, i] {
			this->setShadingMode(static_cast<MDLShadingMode>(i));
		});
		controlsLayout->addWidget(button, 0, Qt::AlignVCenter | Qt::AlignRight);
	}

	controlsLayout->addSpacing(TOOLBAR_SPACE_SIZE);

	auto* tabsToggleButton = new QPushButton(tr("Toggle Info Panel"), controls);
	tabsToggleButton->setCheckable(true);
	tabsToggleButton->setChecked(false);
	QObject::connect(tabsToggleButton, &QPushButton::clicked, this, [&](bool checked) {
		// Only toggle the info panel itself. Its parent is the view container, so hiding the parent would
		// collapse the entire model view.
		this->tabs->setVisible(checked);
		if (checked) {
			// Ensure it has a usable size and is above the GL view.
			this->tabs->adjustSize();
			this->tabs->raise();
		}
	});
	controlsLayout->addWidget(tabsToggleButton);

	// Render view + overlay info panel (does not resize the preview and doesn't block camera controls).
	// Using a grid layout lets the overlay only occupy its own rectangle; the rest of the view still receives mouse input.
	auto* viewContainer = new QWidget(this->preview);
	auto* viewLayout = new QGridLayout(viewContainer);
	viewLayout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(viewContainer);

	this->mdl = new MDLWidget(viewContainer);
	viewLayout->addWidget(this->mdl, 0, 0);

	// Apply and connect grid options now that MDLWidget exists.
	this->mdl->setGridEnabled(Options::get<bool>(OPT_MDL_GRID_ENABLED));
	this->mdl->setGridSpacing(static_cast<float>(Options::get<double>(OPT_MDL_GRID_SPACING)));
	this->mdl->setGridExtentCells(Options::get<int>(OPT_MDL_GRID_EXTENT_CELLS));
	this->mdl->setGridMajorEvery(Options::get<int>(OPT_MDL_GRID_MAJOR_EVERY));
	this->mdl->setGridColors(Options::get<QColor>(OPT_MDL_GRID_MINOR_COLOR), Options::get<QColor>(OPT_MDL_GRID_MAJOR_COLOR));

	QObject::connect(gridToggle, &QCheckBox::toggled, this, [this](bool checked) {
		Options::set(OPT_MDL_GRID_ENABLED, checked);
		if (this->mdl) {
			this->mdl->setGridEnabled(checked);
		}
	});
	QObject::connect(spacingSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
		Options::set(OPT_MDL_GRID_SPACING, v);
		if (this->mdl) {
			this->mdl->setGridSpacing(static_cast<float>(v));
		}
	});
	QObject::connect(extentSpin, &QSpinBox::valueChanged, this, [this](int v) {
		Options::set(OPT_MDL_GRID_EXTENT_CELLS, v);
		if (this->mdl) {
			this->mdl->setGridExtentCells(v);
		}
	});
	QObject::connect(majorEverySpin, &QSpinBox::valueChanged, this, [this](int v) {
		Options::set(OPT_MDL_GRID_MAJOR_EVERY, v);
		if (this->mdl) {
			this->mdl->setGridMajorEvery(v);
		}
	});
	QObject::connect(minorColorBtn, &QPushButton::clicked, this, [this, minorColorBtn, syncColorButton]() {
		const auto current = Options::get<QColor>(OPT_MDL_GRID_MINOR_COLOR);
		const auto picked = QColorDialog::getColor(current, this->preview, tr("Pick Minor Grid Color"), QColorDialog::ShowAlphaChannel);
		if (!picked.isValid()) {
			return;
		}
		Options::set(OPT_MDL_GRID_MINOR_COLOR, picked);
		syncColorButton(minorColorBtn, picked);
		if (this->mdl) {
			this->mdl->setGridColors(picked, Options::get<QColor>(OPT_MDL_GRID_MAJOR_COLOR));
		}
	});
	QObject::connect(majorColorBtn, &QPushButton::clicked, this, [this, majorColorBtn, syncColorButton]() {
		const auto current = Options::get<QColor>(OPT_MDL_GRID_MAJOR_COLOR);
		const auto picked = QColorDialog::getColor(current, this->preview, tr("Pick Major Grid Color"), QColorDialog::ShowAlphaChannel);
		if (!picked.isValid()) {
			return;
		}
		Options::set(OPT_MDL_GRID_MAJOR_COLOR, picked);
		syncColorButton(majorColorBtn, picked);
		if (this->mdl) {
			this->mdl->setGridColors(Options::get<QColor>(OPT_MDL_GRID_MINOR_COLOR), picked);
		}
	});

	this->tabs = new QTabWidget(viewContainer);
	this->tabs->setObjectName("mdlInfoOverlay");
	this->tabs->setMinimumWidth(360);
	this->tabs->setMaximumWidth(520);
	this->tabs->setFixedHeight(360);
	this->tabs->hide();
	this->tabs->setStyleSheet(
		"QTabWidget#mdlInfoOverlay { border: 1px solid rgba(255,255,255,35); border-radius: 6px; }\n"
		"QTabWidget#mdlInfoOverlay::pane { border: none; background-color: rgba(20,20,20,220); border-radius: 6px; }\n"
		"QTabWidget#mdlInfoOverlay QTabBar::tab { padding: 6px 10px; }\n"
		"QTabWidget#mdlInfoOverlay QTreeWidget { background: transparent; border: none; }\n"
		"QTabWidget#mdlInfoOverlay QTreeWidget::viewport { background: transparent; }\n"
	);
	viewLayout->addWidget(this->tabs, 0, 0, Qt::AlignRight | Qt::AlignTop);

	this->materialsTab = new QTreeWidget(this->tabs);
	this->materialsTab->setHeaderHidden(true);
	QObject::connect(this->materialsTab, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
		this->windowAccess->selectEntryInEntryTree(item->text(0));
	});
	this->tabs->addTab(this->materialsTab, tr("Materials Found"));

	this->allMaterialsTab = new QTreeWidget(this->tabs);
	this->allMaterialsTab->setHeaderHidden(true);
	this->tabs->addTab(this->allMaterialsTab, tr("All Materials"));

	this->bodygroupsTab = new QTreeWidget(this->tabs);
	this->bodygroupsTab->setHeaderHidden(true);
	this->bodygroupsTab->setUniformRowHeights(true);
	this->bodygroupsTab->setExpandsOnDoubleClick(false);
	this->bodygroupsTab->setIndentation(18);
	this->tabs->addTab(this->bodygroupsTab, tr("Bodygroups"));
	// Use itemChanged to properly respect checkbox toggling and enforce exclusivity per bodypart.
	QObject::connect(this->bodygroupsTab, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
		(void)column;
		if (this->updatingBodygroupsTab) {
			return;
		}
		if (!item || !item->parent()) {
			return;
		}
		if (!this->cachedRespawnVTX || !this->cachedRespawnVVD || !this->cachedRespawnMDL) {
			return;
		}
		// Only react when something is checked/unchecked.
		const auto state = item->checkState(0);
		if (state != Qt::CheckState::Checked && state != Qt::CheckState::Unchecked) {
			return;
		}

		bool okBP = false;
		bool okModel = false;
		const int bpIndex = item->data(0, Qt::UserRole).toInt(&okBP);
		const int modelIndex = item->data(0, Qt::UserRole + 1).toInt(&okModel);
		if (!okBP || !okModel) {
			return;
		}
		if (bpIndex < 0 || bpIndex >= static_cast<int>(this->respawnBodygroupSelection.size())) {
			return;
		}

		const int prev = this->respawnBodygroupSelection[static_cast<std::size_t>(bpIndex)];

		int next = prev;
		if (state == Qt::CheckState::Checked) {
			next = modelIndex; // can be -1 for disabled
		} else {
			// Unchecking the currently-selected item disables the bodypart.
			if (prev != modelIndex) {
				return;
			}
			next = -1;
		}

		if (prev != next) {
			this->respawnBodygroupSelection[static_cast<std::size_t>(bpIndex)] = next;
			if (!this->rebuildRespawnModelFromCache()) {
				this->respawnBodygroupSelection[static_cast<std::size_t>(bpIndex)] = prev;
				next = prev;
			}
		}

		// Enforce exclusivity in-place; do not clear/repopulate the tree inside this signal (can crash Qt).
		{
			const QSignalBlocker blocker(this->bodygroupsTab);
			auto* parent = item->parent();
			for (int i = 0; i < parent->childCount(); i++) {
				auto* child = parent->child(i);
				if (!child) {
					continue;
				}
				bool okChildModel = false;
				const int childModelIndex = child->data(0, Qt::UserRole + 1).toInt(&okChildModel);
				if (!okChildModel) {
					continue;
				}
				child->setCheckState(0, (childModelIndex == next) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
			}
		}

		this->mdl->update();
	});
}

QWidget * MDLPreview::getPreview() const {
	return this->preview;
}

const QSet<QString>& MDLPreview::getPreviewExtensions() const {
	static const QSet<QString> EXTENSIONS{
		".mdl",
		".vtx",
		".vvd",
		".phy",
		".ani",
		".vta",
	};
	return EXTENSIONS;
}

QIcon MDLPreview::getIcon() const {
	// todo: cool icon
	return {};
}

int MDLPreview::setData(const QString& path, const quint8* dataPtr, quint64 length) {
	this->mdl->clearMeshes();
	this->cachedRespawnMDL.reset();
	this->cachedRespawnVTX.reset();
	this->cachedRespawnVVD.reset();
	this->respawnBodygroupSelection.clear();
	this->respawnCameraInitialized = false;
	if (this->bodygroupsTab) {
		this->bodygroupsTab->clear();
	}

	std::string basePath = std::filesystem::path{path.toLocal8Bit().constData()}.replace_extension().string();
	if (path.endsWith(".vtx")) {
		// Remove .dx80, .dx90, .sw
		basePath = std::filesystem::path{basePath}.replace_extension().string();
	}

	// Prefer the provided buffer for the file being previewed. This matters when previewing loose files
	// (outside of a loaded archive), and avoids a redundant read when previewing an entry inside an archive.
	const auto pathLower = path.toLower();
	QByteArray mdlData;
	QByteArray vvdData;
	QByteArray vtxData;
	bool hasMDLData = false;
	bool hasVVDData = false;
	bool hasVTXData = false;

	const auto provided = QByteArray{reinterpret_cast<const char*>(dataPtr), static_cast<qsizetype>(length)};
	if (pathLower.endsWith(".mdl")) {
		mdlData = provided;
		hasMDLData = true;
	} else if (pathLower.endsWith(".vvd")) {
		vvdData = provided;
		hasVVDData = true;
	} else if (pathLower.endsWith(".vtx") || pathLower.contains(".dx11.vtx") || pathLower.contains(".dx90.vtx") || pathLower.contains(".dx80.vtx") || pathLower.contains(".sw.vtx")) {
		vtxData = provided;
		hasVTXData = true;
	}

	// If we didn't get the required pieces via the provided buffer:
	// 1) If `path` is a real filesystem path, try loading sidecars from disk next to it.
	// 2) Otherwise, attempt to load sidecars from the current archive.
	const std::filesystem::path fsPath{path.toStdWString()};
	const bool isLooseFile = std::filesystem::exists(fsPath) && std::filesystem::is_regular_file(fsPath);
	auto readFileBytes = [](const std::filesystem::path& p, QByteArray& out) -> bool {
		QFile f(QString::fromStdWString(p.wstring()));
		if (!f.open(QIODevice::ReadOnly)) {
			return false;
		}
		out = f.readAll();
		return !out.isEmpty();
	};

	if (isLooseFile) {
		const std::filesystem::path baseFsPath = fsPath.parent_path() / fsPath.stem();
		if (!hasMDLData && fsPath.extension() == L".mdl") {
			auto p = baseFsPath;
			p += L".mdl";
			hasMDLData = readFileBytes(p, mdlData);
		}
		if (!hasVVDData) {
			auto p = baseFsPath;
			p += L".vvd";
			hasVVDData = readFileBytes(p, vvdData);
		}
		if (!hasVTXData) {
			for (const auto* ext : {L".vtx", L".dx11.vtx", L".dx90.vtx", L".dx80.vtx", L".sw.vtx"}) {
				auto p = baseFsPath;
				p += ext;
				hasVTXData = readFileBytes(p, vtxData);
				if (hasVTXData) {
					break;
				}
			}
		}
	}

	// Fallback: read from archive if available.
	if (!hasMDLData) {
		hasMDLData = this->windowAccess->readBinaryEntry((basePath + ".mdl").c_str(), mdlData);
	}
	if (!hasVVDData) {
		hasVVDData = this->windowAccess->readBinaryEntry((basePath + ".vvd").c_str(), vvdData);
	}
	if (!hasVTXData) {
		for (const auto* ext : {".vtx", ".dx11.vtx", ".dx90.vtx", ".dx80.vtx", ".sw.vtx"}) {
			hasVTXData = this->windowAccess->readBinaryEntry((basePath + ext).c_str(), vtxData);
			if (hasVTXData) {
				break;
			}
		}
	}

	const std::byte* finalMDLPtr = reinterpret_cast<const std::byte*>(mdlData.data());
	std::size_t finalMDLSize = static_cast<std::size_t>(mdlData.size());

	const std::byte* finalVVDPtr = hasVVDData ? reinterpret_cast<const std::byte*>(vvdData.data()) : nullptr;
	std::size_t finalVVDSize = hasVVDData ? static_cast<std::size_t>(vvdData.size()) : 0;

	const std::byte* finalVTXPtr = hasVTXData ? reinterpret_cast<const std::byte*>(vtxData.data()) : nullptr;
	std::size_t finalVTXSize = hasVTXData ? static_cast<std::size_t>(vtxData.size()) : 0;

	// Titanfall 2 models can be a single file; attempt to find embedded VVD/VTX when sidecars are missing.
	if (hasMDLData && (!hasVVDData || !hasVTXData)) {
		MDL::MDL mdlHeader;
		if (mdlHeader.open(finalMDLPtr, finalMDLSize)) {
			const auto embedded = findEmbeddedModelBuffers(finalMDLPtr, finalMDLSize, mdlHeader);
			if (!finalVVDPtr && embedded.vvdData) {
				finalVVDPtr = embedded.vvdData;
				finalVVDSize = embedded.vvdSize;
			}
			if (!finalVTXPtr && embedded.vtxData) {
				finalVTXPtr = embedded.vtxData;
				finalVTXSize = embedded.vtxSize;
			}
		}
	}

	if (!hasMDLData || !finalVVDPtr || !finalVTXPtr) {
		QString error{tr("Unable to find all the required files the model is composed of!") + '\n'};
		if (!hasMDLData) {
			error += "\n- ";
			error += basePath.c_str();
			error += ".mdl";
		}
		if (!finalVVDPtr) {
			error += "\n- ";
			error += basePath.c_str();
			error += ".vvd";
		}
		if (!finalVTXPtr) {
			error += "\n- " + tr("One of the following:") +
					 "\n  - " + basePath.c_str() + ".vtx" +
					 "\n  - " + basePath.c_str() + ".dx11.vtx" +
					 "\n  - " + basePath.c_str() + ".dx90.vtx" +
					 "\n  - " + basePath.c_str() + ".dx80.vtx" +
					 "\n  - " + basePath.c_str() + ".sw.vtx";
			error += "\n\n" + tr("Note: Some games (e.g., Titanfall 2) may embed this data inside the .mdl. If this model is a single file and still won't preview, please export it to loose files first.");
		}

		emit this->showGenericErrorPreview(error);
		return ERROR_SHOWED_OTHER_PREVIEW;
	}

	MDL::MDL mdlHeader;
	if (!mdlHeader.open(finalMDLPtr, finalMDLSize)) {
		emit this->showGenericErrorPreview(tr("This model is invalid, it cannot be previewed!"));
		return ERROR_SHOWED_OTHER_PREVIEW;
	}

	// Respawn/Titanfall models use newer MDL versions that arent fully parsed
	// We can still preview them by using VVD/VTX directly. We also parse the texture/skin/bodypart tables
	// from the v53 header so we can bind materials when they exist in the archive
	if (mdlHeader.version > 49) {
		auto vtxParsed = std::make_unique<VTX::VTX>();
		if (!vtxParsed->open(finalVTXPtr, finalVTXSize, mdlHeader)) {
			emit this->showGenericErrorPreview(tr("This model is invalid, it cannot be previewed!"));
			return ERROR_SHOWED_OTHER_PREVIEW;
		}
		auto vvdParsed = std::make_unique<VVD::VVD>();
		if (!vvdParsed->open(finalVVDPtr, finalVVDSize, mdlHeader)) {
			emit this->showGenericErrorPreview(tr("This model is invalid, it cannot be previewed!"));
			return ERROR_SHOWED_OTHER_PREVIEW;
		}

		// Cache parsed structs for fast bodygroup toggles.
		this->cachedRespawnMDL = std::make_unique<MDL::MDL>(mdlHeader);
		this->cachedRespawnVTX = std::move(vtxParsed);
		this->cachedRespawnVVD = std::move(vvdParsed);

		this->respawnBodygroupSelection.assign(this->cachedRespawnVTX->bodyParts.size(), 0);
		for (std::size_t bp = 0; bp < this->cachedRespawnVTX->bodyParts.size(); bp++) {
			if (this->cachedRespawnVTX->bodyParts[bp].models.empty()) {
				this->respawnBodygroupSelection[bp] = -1;
			}
		}

		if (!this->rebuildRespawnModelFromCache()) {
			emit this->showGenericErrorPreview(tr("This model is invalid, it cannot be previewed!"));
			return ERROR_SHOWED_OTHER_PREVIEW;
		}

		// Skins: if the MDL doesnt have a skin table, fall back to an identity mapping so the shader can bind textures
		auto skins = mdlHeader.skins;
		if (skins.empty() && !mdlHeader.materials.empty()) {
			std::vector<int16_t> identity;
			identity.reserve(mdlHeader.materials.size());
			for (int i = 0; i < static_cast<int>(mdlHeader.materials.size()); i++) {
				identity.push_back(static_cast<int16_t>(i));
			}
			skins.push_back(std::move(identity));
		}

		this->skinSpinBox->setValue(0);
		this->skinSpinBox->setMaximum(std::max(static_cast<int>(skins.size()) - 1, 0));
		this->skinSpinBox->setDisabled(this->skinSpinBox->maximum() == 0);
		this->mdl->setSkinLookupTable(std::move(skins));

		// Initialize camera framing once, then keep it stable across bodygroup toggles.
		if (!this->respawnCameraInitialized) {
			// Build a basic AABB from the current baked vertices by reading back what we just set.
			// We don't have direct access to baked data here, so rebuild once with an AABB computed inside rebuildRespawnModelFromCache.
			// (rebuildRespawnModelFromCache sets AABB only on first run.)
			this->respawnCameraInitialized = true;
		}

		this->populateBodygroupsTab();

		// Populate material info panels
		this->allMaterialsTab->clear();
		auto* allMaterialDirsItem = new QTreeWidgetItem(this->allMaterialsTab);
		allMaterialDirsItem->setText(0, tr("Folders"));
		this->allMaterialsTab->addTopLevelItem(allMaterialDirsItem);
		for (const auto& materialDir : mdlHeader.materialDirectories) {
			auto* materialDirItem = new QTreeWidgetItem(allMaterialDirsItem);
			materialDirItem->setText(0, QString{materialDir.c_str()}.toLower());
		}
		allMaterialDirsItem->setExpanded(true);

		auto* allMaterialNamesItem = new QTreeWidgetItem(this->allMaterialsTab);
		allMaterialNamesItem->setText(0, tr("Material Names"));
		this->allMaterialsTab->addTopLevelItem(allMaterialNamesItem);
		for (const auto& material : mdlHeader.materials) {
			auto* materialNameItem = new QTreeWidgetItem(allMaterialNamesItem);
			materialNameItem->setText(0, QString{material.name.c_str()}.toLower());
		}
		allMaterialNamesItem->setExpanded(true);

		// Add the materials that actually exist (and their base textures) to the found materials panel
		this->materialsTab->clear();
		std::vector<std::unique_ptr<MDLTextureData>> vtfs;
		bool foundAnyMaterials = false;
		vtfs.reserve(mdlHeader.materials.size());
		for (int materialIndex = 0; materialIndex < static_cast<int>(mdlHeader.materials.size()); materialIndex++) {
			bool foundMaterial = false;
			for (int materialDirIndex = 0; materialDirIndex < static_cast<int>(mdlHeader.materialDirectories.size()); materialDirIndex++) {
				std::string vmtPath = "materials/"s + mdlHeader.materialDirectories.at(materialDirIndex) + mdlHeader.materials.at(materialIndex).name + ".vmt";
				string::normalizeSlashes(vmtPath);
				string::toLower(vmtPath);
				if (auto data = ::getTextureDataForMaterial(this->windowAccess, vmtPath)) {
					vtfs.push_back(std::move(data));

					auto* item = new QTreeWidgetItem(this->materialsTab);
					item->setText(0, vmtPath.c_str());
					this->materialsTab->addTopLevelItem(item);

					foundMaterial = true;
					break;
				}
			}
			if (!foundMaterial) {
				vtfs.emplace_back(nullptr);
			}
			foundAnyMaterials = foundAnyMaterials || foundMaterial;
		}
		this->mdl->setTextures(vtfs);

		if (foundAnyMaterials) {
			this->setShadingMode(MDLShadingMode::SHADED_TEXTURED);
		} else {
			this->setShadingMode(MDLShadingMode::SHADED_UNTEXTURED);
		}
		this->mdl->update();

		return ERROR_SHOWED_THIS_PREVIEW;
	}

	StudioModel mdlParser;
	const bool opened = mdlParser.open(
		finalMDLPtr, finalMDLSize,
		finalVTXPtr, finalVTXSize,
		finalVVDPtr, finalVVDSize);
	if (!opened) {
		emit this->showGenericErrorPreview(tr("This model is invalid, it cannot be previewed!"));
		return ERROR_SHOWED_OTHER_PREVIEW;
	}

	// Maybe we can add a setting for LOD...
	const auto bakedModel = mdlParser.processModelData(ROOT_LOD);
	this->mdl->setModel(bakedModel);

	this->skinSpinBox->setValue(0);
	this->skinSpinBox->setMaximum(std::max(static_cast<int>(mdlParser.mdl.skins.size()) - 1, 0));
	this->skinSpinBox->setDisabled(this->skinSpinBox->maximum() == 0);
	this->mdl->setSkinLookupTable(mdlParser.mdl.skins);

	this->mdl->setAABB({
		{mdlParser.mdl.hullMin[0], mdlParser.mdl.hullMin[1], mdlParser.mdl.hullMin[2]},
		{mdlParser.mdl.hullMax[0], mdlParser.mdl.hullMax[1], mdlParser.mdl.hullMax[2]},
	});

	// Add material directories and names to the material names panel
	this->allMaterialsTab->clear();
	auto* allMaterialDirsItem = new QTreeWidgetItem(this->allMaterialsTab);
	allMaterialDirsItem->setText(0, tr("Folders"));
	this->allMaterialsTab->addTopLevelItem(allMaterialDirsItem);
	for (const auto& materialDir : mdlParser.mdl.materialDirectories) {
		auto* materialDirItem = new QTreeWidgetItem(allMaterialDirsItem);
		materialDirItem->setText(0, QString{materialDir.c_str()}.toLower());
	}
	allMaterialDirsItem->setExpanded(true);
	auto* allMaterialNamesItem = new QTreeWidgetItem(this->allMaterialsTab);
	allMaterialNamesItem->setText(0, tr("Material Names"));
	this->allMaterialsTab->addTopLevelItem(allMaterialNamesItem);
	for (const auto& material : mdlParser.mdl.materials) {
		auto* materialNameItem = new QTreeWidgetItem(allMaterialNamesItem);
		materialNameItem->setText(0, QString{material.name.c_str()}.toLower());
	}
	allMaterialNamesItem->setExpanded(true);

	// Add the materials that actually exist to the found materials panel
	this->materialsTab->clear();
	std::vector<std::unique_ptr<MDLTextureData>> vtfs;
	bool foundAnyMaterials = false;
	for (int materialIndex = 0; materialIndex < mdlParser.mdl.materials.size(); materialIndex++) {
		bool foundMaterial = false;
		for (int materialDirIndex = 0; materialDirIndex < mdlParser.mdl.materialDirectories.size(); materialDirIndex++) {
			std::string vmtPath = "materials/"s + mdlParser.mdl.materialDirectories.at(materialDirIndex) + mdlParser.mdl.materials.at(materialIndex).name + ".vmt";
			string::normalizeSlashes(vmtPath);
			string::toLower(vmtPath);
			if (auto data = ::getTextureDataForMaterial(this->windowAccess, vmtPath)) {
				vtfs.push_back(std::move(data));

				auto* item = new QTreeWidgetItem(this->materialsTab);
				item->setText(0, vmtPath.c_str());
				this->materialsTab->addTopLevelItem(item);

				foundMaterial = true;
				break;
			}
		}
		if (!foundMaterial) {
			vtfs.emplace_back(nullptr);
		}
		foundAnyMaterials = foundAnyMaterials || foundMaterial;
	}
	this->mdl->setTextures(vtfs);

	if (foundAnyMaterials) {
		this->setShadingMode(MDLShadingMode::SHADED_TEXTURED);
	} else {
		this->setShadingMode(MDLShadingMode::SHADED_UNTEXTURED);
	}
	this->mdl->update();

	return ERROR_SHOWED_THIS_PREVIEW;
}

bool MDLPreview::rebuildRespawnModelFromCache() {
	if (!this->cachedRespawnMDL || !this->cachedRespawnVTX || !this->cachedRespawnVVD) {
		return false;
	}

	const auto& mdlHeader = *this->cachedRespawnMDL;
	const auto& vtxParsed = *this->cachedRespawnVTX;
	const auto& vvdParsed = *this->cachedRespawnVVD;

	// Some larger/skinned models rely on VVD fixups. In that case, VTX meshVertexID often indexes into the
	// per-LOD vertex table (not the raw/global VVD vertex array), so we need to remap to source vertex IDs
	std::vector<std::uint32_t> lod0VertexRemap;
	if (!vvdParsed.fixups.empty()) {
		lod0VertexRemap.reserve(static_cast<std::size_t>(vvdParsed.numVerticesInLOD[0]));
		for (const auto& fx : vvdParsed.fixups) {
			if (fx.LOD != 0) {
				continue;
			}
			const std::size_t start = static_cast<std::size_t>(fx.sourceVertexID);
			const std::size_t count = static_cast<std::size_t>(fx.vertexCount);
			for (std::size_t i = 0; i < count; i++) {
				lod0VertexRemap.push_back(static_cast<std::uint32_t>(start + i));
			}
		}

		// If this doesn't match what the file claims for LOD0, don't apply it.
		if (lod0VertexRemap.size() != static_cast<std::size_t>(vvdParsed.numVerticesInLOD[0])) {
			lod0VertexRemap.clear();
		}
	}

	// Valve mstudiovertex_t is 48 bytes. Studio MDL stores `vertexindex` as a byte offset into the
	// global vertex array, while VTX vertex IDs are indices into the model/mesh-local vertex range
	static constexpr std::size_t kVVDVertexStride = 48;

	BakedModel baked;
	baked.vertices.reserve(vvdParsed.vertices.size());
	for (const auto& v : vvdParsed.vertices) {
		baked.vertices.push_back({v.position, v.normal, v.uv});
	}

	for (std::size_t bodyPartIndex = 0; bodyPartIndex < vtxParsed.bodyParts.size(); bodyPartIndex++) {
		const auto& bodyPart = vtxParsed.bodyParts[bodyPartIndex];
		if (bodyPart.models.empty()) {
			continue;
		}

		int sel = 0;
		if (bodyPartIndex < this->respawnBodygroupSelection.size()) {
			sel = this->respawnBodygroupSelection[bodyPartIndex];
		}
		if (sel < 0 || sel >= static_cast<int>(bodyPart.models.size())) {
			continue;
		}

		const std::size_t modelIndex = static_cast<std::size_t>(sel);
		const auto& model = bodyPart.models[modelIndex];
		if (model.modelLODs.empty()) {
			continue;
		}

		const auto& lod = model.modelLODs.front();
		std::size_t meshIndex = 0;
		for (const auto& mesh : lod.meshes) {
			auto computeBaseVertexOffset = [&]() -> std::size_t {
				std::size_t baseVertexOffset = 0;
				if (bodyPartIndex < mdlHeader.bodyParts.size()) {
					const auto& mdlBP = mdlHeader.bodyParts[bodyPartIndex];
					if (modelIndex < mdlBP.models.size()) {
						const auto& mdlModel = mdlBP.models[modelIndex];
						if (mdlModel.verticesOffset >= 0) {
							baseVertexOffset = static_cast<std::size_t>(mdlModel.verticesOffset) / kVVDVertexStride;
						}
						if (meshIndex < mdlModel.meshes.size()) {
							const auto& mdlMesh = mdlModel.meshes[meshIndex];
							if (mdlMesh.verticesOffset > 0) {
								baseVertexOffset += static_cast<std::size_t>(mdlMesh.verticesOffset);
							}
						}
					}
				}
				return baseVertexOffset;
			};

			auto buildIndices = [&](auto&& mapVertexId) -> std::optional<std::vector<uint16_t>> {
				std::vector<uint16_t> indices;
				for (const auto& stripGroup : mesh.stripGroups) {
					for (const auto& strip : stripGroup.strips) {
						bool invalidIndex = false;

						const auto addIndex = [&](uint16_t stripVertexIndex) {
							if (invalidIndex) {
								return;
							}
							const std::size_t vtxVertIndex = static_cast<std::size_t>(stripVertexIndex);
							if (vtxVertIndex >= stripGroup.vertices.size()) {
								invalidIndex = true;
								return;
							}
							const std::size_t meshVertexIDLocal = static_cast<std::size_t>(stripGroup.vertices[vtxVertIndex].meshVertexID);
							const std::optional<std::size_t> mapped = mapVertexId(meshVertexIDLocal);
							if (!mapped) {
								invalidIndex = true;
								return;
							}
							const std::size_t meshVertexID = *mapped;
							if (meshVertexID >= baked.vertices.size()) {
								invalidIndex = true;
								return;
							}
							if (meshVertexID > std::numeric_limits<uint16_t>::max()) {
								invalidIndex = true;
								return;
							}
							indices.push_back(static_cast<uint16_t>(meshVertexID));
						};

						if (strip.flags & VTX::Strip::FLAG_IS_TRILIST) {
							for (std::size_t i = 0; i + 2 < strip.indices.size(); i += 3) {
								addIndex(strip.indices[i]);
								addIndex(strip.indices[i + 2]);
								addIndex(strip.indices[i + 1]);
							}
						} else {
							for (std::size_t i = 0; i + 2 < strip.indices.size(); ++i) {
								const auto a = strip.indices[i];
								const auto b = strip.indices[i + 1];
								const auto c = strip.indices[i + 2];
								if (a == b || a == c || b == c) {
									continue;
								}
								const bool flip = (i % 2) == 1;
								addIndex(flip ? b : a);
								addIndex(flip ? a : b);
								addIndex(c);
							}
						}

						if (invalidIndex) {
							return std::nullopt;
						}
					}
				}
				return indices;
			};

			std::optional<std::vector<uint16_t>> indices;
			if (!lod0VertexRemap.empty()) {
				indices = buildIndices([&](std::size_t local) -> std::optional<std::size_t> {
					if (local >= lod0VertexRemap.size()) {
						return std::nullopt;
					}
					return static_cast<std::size_t>(lod0VertexRemap[local]);
				});
			}
			if (!indices) {
				const auto base = computeBaseVertexOffset();
				indices = buildIndices([&](std::size_t local) -> std::optional<std::size_t> {
					const auto mapped = base + local;
					if (mapped < base) {
						return std::nullopt;
					}
					return mapped;
				});
			}
			if (!indices) {
				indices = buildIndices([&](std::size_t local) -> std::optional<std::size_t> { return local; });
			}
			if (!indices) {
				// skip this mesh rather than crashing or blanking the whole model
				meshIndex++;
				continue;
			}

			int materialIndex = -1;
			if (bodyPartIndex < mdlHeader.bodyParts.size()) {
				const auto& mdlBP = mdlHeader.bodyParts[bodyPartIndex];
				if (modelIndex < mdlBP.models.size()) {
					const auto& mdlModel = mdlBP.models[modelIndex];
					if (meshIndex < mdlModel.meshes.size()) {
						materialIndex = mdlModel.meshes[meshIndex].material;
					}
				}
			}

			baked.meshes.push_back({std::move(*indices), materialIndex});
			meshIndex++;
		}
	}

	if (baked.meshes.empty() || baked.vertices.empty()) {
		return false;
	}

	this->mdl->setModel(baked);

	// Only frame the camera once per opened model; bodygroup toggles should not reset the view.
	if (!this->respawnCameraInitialized) {
		auto mn = QVector3D{baked.vertices[0].position[0], baked.vertices[0].position[1], baked.vertices[0].position[2]};
		auto mx = mn;
		for (const auto& v : baked.vertices) {
			mn.setX(std::min(mn.x(), v.position[0]));
			mn.setY(std::min(mn.y(), v.position[1]));
			mn.setZ(std::min(mn.z(), v.position[2]));
			mx.setX(std::max(mx.x(), v.position[0]));
			mx.setY(std::max(mx.y(), v.position[1]));
			mx.setZ(std::max(mx.z(), v.position[2]));
		}
		this->mdl->setAABB({mn, mx});
		this->respawnCameraInitialized = true;
	}
	return true;
}

void MDLPreview::populateBodygroupsTab() {
	if (!this->bodygroupsTab) {
		return;
	}

	this->updatingBodygroupsTab = true;
	const QSignalBlocker blocker(this->bodygroupsTab);
	this->bodygroupsTab->clear();

	if (!this->cachedRespawnVTX || !this->cachedRespawnMDL) {
		this->updatingBodygroupsTab = false;
		return;
	}

	const auto& vtxParsed = *this->cachedRespawnVTX;
	const auto& mdlHeader = *this->cachedRespawnMDL;

	for (std::size_t bpIndex = 0; bpIndex < vtxParsed.bodyParts.size(); bpIndex++) {
		QString bpName = tr("Bodypart %1").arg(static_cast<int>(bpIndex));
		if (bpIndex < mdlHeader.bodyParts.size() && !mdlHeader.bodyParts[bpIndex].name.empty()) {
			bpName = QString::fromStdString(mdlHeader.bodyParts[bpIndex].name);
		}

		auto* bpItem = new QTreeWidgetItem(this->bodygroupsTab);
		bpItem->setText(0, bpName);
		this->bodygroupsTab->addTopLevelItem(bpItem);

		const int selected = (bpIndex < this->respawnBodygroupSelection.size()) ? this->respawnBodygroupSelection[bpIndex] : 0;

		const auto& bodyPart = vtxParsed.bodyParts[bpIndex];

		// Disabled option (lets user hide this entire bodypart).
		{
			auto* disabledItem = new QTreeWidgetItem(bpItem);
			disabledItem->setText(0, tr("(Disabled)"));
			disabledItem->setData(0, Qt::UserRole, static_cast<int>(bpIndex));
			disabledItem->setData(0, Qt::UserRole + 1, -1);
			disabledItem->setFlags(disabledItem->flags() | Qt::ItemIsUserCheckable);
			disabledItem->setCheckState(0, (selected < 0) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
		}

		for (std::size_t modelIndex = 0; modelIndex < bodyPart.models.size(); modelIndex++) {
			QString modelName = tr("Model %1").arg(static_cast<int>(modelIndex));
			if (bpIndex < mdlHeader.bodyParts.size()) {
				const auto& mdlBP = mdlHeader.bodyParts[bpIndex];
				if (modelIndex < mdlBP.models.size() && !mdlBP.models[modelIndex].name.empty()) {
					modelName = QString::fromStdString(mdlBP.models[modelIndex].name);
				}
			}

			auto* modelItem = new QTreeWidgetItem(bpItem);
			modelItem->setText(0, modelName);
			modelItem->setData(0, Qt::UserRole, static_cast<int>(bpIndex));
			modelItem->setData(0, Qt::UserRole + 1, static_cast<int>(modelIndex));
			modelItem->setFlags(modelItem->flags() | Qt::ItemIsUserCheckable);
			modelItem->setCheckState(0, (static_cast<int>(modelIndex) == selected) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
		}

		bpItem->setExpanded(true);
	}

	this->updatingBodygroupsTab = false;
}

void MDLPreview::setShadingMode(MDLShadingMode mode) const {
	this->backfaceCulling->setDisabled(mode == MDLShadingMode::WIREFRAME);

	const QList<std::tuple<QToolButton* const*, QString, MDLShadingMode>> buttonsAndIcons{
			{&this->shadingModeWireframe, ":/icons/model_wireframe.png", MDLShadingMode::WIREFRAME},
			{&this->shadingModeShadedUntextured, ":/icons/model_shaded_untextured.png", MDLShadingMode::SHADED_UNTEXTURED},
			{&this->shadingModeUnshadedTextured, ":/icons/model_unshaded_textured.png", MDLShadingMode::UNSHADED_TEXTURED},
			{&this->shadingModeShadedTextured, ":/icons/model_shaded_textured.png", MDLShadingMode::SHADED_TEXTURED},
	};
	for (auto& [button, iconPath, buttonMode] : buttonsAndIcons) {
		(*button)->setIcon(ThemedIcon::get(this->preview, iconPath, buttonMode == mode ? QPalette::ColorRole::Link : QPalette::ColorRole::ButtonText));
		(*button)->setIconSize({SHADING_MODE_BUTTON_SIZE, SHADING_MODE_BUTTON_SIZE});
	}

	this->mdl->setShadingMode(mode);
}
