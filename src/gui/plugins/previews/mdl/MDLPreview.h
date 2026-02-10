#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include <QBasicTimer>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QQuaternion>
#include <QVector3D>
#include <QColor>

#include <mdlpp/mdlpp.h>

#include "../IVPKEditPreviewPlugin.h"

// NOTE: We include <mdlpp/mdlpp.h> here because we store parsed MDL/VTX/VVD objects in std::unique_ptrs.
// unique_ptr's destructor is inline, so incomplete forward-declared types will fail to compile on MSVC.

class QCheckBox;
class QKeyEvent;
class QMouseEvent;
class QSpinBox;
class QTabWidget;
class QTimerEvent;
class QToolButton;
class QTreeWidget;

struct AABB {
	[[nodiscard]] QList<QVector3D> getCorners() const;

	[[nodiscard]] float getWidth() const;

	[[nodiscard]] float getHeight() const;

	[[nodiscard]] float getDepth() const;

	QVector3D min;
	QVector3D max;
};

struct MDLSubMesh {
	std::unique_ptr<QOpenGLVertexArrayObject> vao;
	int textureIndex;
	QOpenGLBuffer ebo{QOpenGLBuffer::Type::IndexBuffer};
	int indexCount;
};

enum class MDLShadingMode {
	WIREFRAME = 0,
	SHADED_UNTEXTURED = 1,
	UNSHADED_TEXTURED = 2,
	SHADED_TEXTURED = 3,
};

struct MDLTextureSettings {
	enum class TransparencyMode {
		NONE,
		ALPHA_TEST,
		TRANSLUCENT,
	} transparencyMode = TransparencyMode::NONE;
	float alphaTestReference = 0.7f;
};

struct MDLTextureData {
	std::vector<std::byte> data;
	std::uint16_t width;
	std::uint16_t height;
	MDLTextureSettings settings;
};

class MDLWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
	Q_OBJECT;

public:
	explicit MDLWidget(QWidget* parent = nullptr);

	~MDLWidget() override;

	void setModel(const mdlpp::BakedModel& model);

	void setTextures(const std::vector<std::unique_ptr<MDLTextureData>>& vtfData);

	void clearTextures();

	void setSkinLookupTable(std::vector<std::vector<short>> skins_);

	void setAABB(AABB aabb);

	[[nodiscard]] int getSkin() const { return this->skin; }

	void setSkin(int skin_);

	[[nodiscard]] MDLShadingMode getShadingMode() const { return this->shadingMode; }

	void setShadingMode(MDLShadingMode type);

	[[nodiscard]] float getFieldOfView() const { return this->fov; };

	void setFieldOfView(float newFOV);

	[[nodiscard]] bool isCullBackFaces() const { return this->cullBackFaces; };

	void setCullBackFaces(bool enable);

	void clearMeshes();

	// Grid (XZ plane at origin)
	void setGridEnabled(bool enable);
	void setGridSpacing(float spacing);
	void setGridExtentCells(int extentCells);
	void setGridMajorEvery(int majorEvery);
	void setGridColors(const QColor& minorColor, const QColor& majorColor);

protected:
	void initializeGL() override;

	void resizeGL(int w, int h) override;

	void paintGL() override;

	void mousePressEvent(QMouseEvent* event) override;

	void mouseReleaseEvent(QMouseEvent* event) override;

	void mouseMoveEvent(QMouseEvent* event) override;

	void wheelEvent(QWheelEvent* event) override;

	void timerEvent(QTimerEvent* event) override;

private:
	enum class InteractionMode {
		NONE,
		ORBIT,
		PAN,
		DOLLY,
	};

	QOpenGLShaderProgram wireframeShaderProgram;
	QOpenGLShaderProgram shadedUntexturedShaderProgram;
	QOpenGLShaderProgram unshadedTexturedShaderProgram;
	QOpenGLShaderProgram shadedTexturedShaderProgram;
	QOpenGLTexture missingTexture;
	QOpenGLTexture matCapTexture;
	QOpenGLShaderProgram gridShaderProgram;
	QOpenGLVertexArrayObject gridVao;
	QOpenGLBuffer gridVertices{QOpenGLBuffer::Type::VertexBuffer};
	int gridVertexCount = 0;
	QOpenGLBuffer vertices{QOpenGLBuffer::Type::VertexBuffer};
	int vertexCount;
	std::vector<MDLSubMesh> meshes;
	QList<QPair<QOpenGLTexture*, MDLTextureSettings>> textures;

	int skin;
	std::vector<std::vector<short>> skins;

	MDLShadingMode shadingMode;
	QMatrix4x4 projection;
	float distance;
	float distanceScale;
	QVector3D target;
	float fov;
	bool cullBackFaces;

	QBasicTimer timer;
	QVector2D mousePressPosition;
	QVector3D rotationAxis;
	QVector3D translationalVelocity;
	qreal angularSpeed;
	QQuaternion rotation;
	InteractionMode interactionMode = InteractionMode::NONE;
	bool rmbBeingHeld;

	// Orbit controls: stable yaw/pitch while dragging
	float orbitYawDeg = 0.0f;
	float orbitPitchDeg = 0.0f;

	// Grid settings
	bool gridEnabled = false;
	float gridSpacing = 32.0f;
	int gridExtentCells = 10;
	int gridMajorEvery = 2;
	QColor gridMinorColor{80, 80, 80, 180};
	QColor gridMajorColor{130, 130, 130, 220};

	void rebuildGridGeometry();
};

class MDLPreview final : public IVPKEditPreviewPlugin_V1_3 {
	Q_OBJECT;
#if !defined(VPKEDIT_BUILTIN_PREVIEW_PLUGINS)
	Q_PLUGIN_METADATA(IID IVPKEditPreviewPlugin_V1_3_iid FILE "MDLPreview.json");
	Q_INTERFACES(IVPKEditPreviewPlugin_V1_3);
#endif

public:
	~MDLPreview() override;

	void initPlugin(IVPKEditWindowAccess_V3* windowAccess_) override;

	void initPreview(QWidget* parent) override;

	[[nodiscard]] QWidget* getPreview() const override;

	[[nodiscard]] const QSet<QString>& getPreviewExtensions() const override;

	[[nodiscard]] QIcon getIcon() const override;

	[[nodiscard]] int setData(const QString& path, const quint8* dataPtr, quint64 length) override;

	void initContextMenu(int, QMenu*) override {}

	void updateContextMenu(int, const QStringList&) override {}

private:
	bool rebuildRespawnModelFromCache();
	void populateBodygroupsTab();
	void setShadingMode(MDLShadingMode mode) const;

	IVPKEditWindowAccess_V3* windowAccess = nullptr;
	QWidget* preview = nullptr;

	QCheckBox* backfaceCulling = nullptr;
	QSpinBox* skinSpinBox = nullptr;
	QToolButton* shadingModeWireframe = nullptr;
	QToolButton* shadingModeShadedUntextured = nullptr;
	QToolButton* shadingModeUnshadedTextured = nullptr;
	QToolButton* shadingModeShadedTextured = nullptr;

	MDLWidget* mdl = nullptr;

	QTabWidget* tabs = nullptr;
	QTreeWidget* materialsTab = nullptr;
	QTreeWidget* allMaterialsTab = nullptr;
	QTreeWidget* bodygroupsTab = nullptr;

	// Respawn/Titanfall (MDL v53+) caching for bodygroup toggles.
	std::unique_ptr<mdlpp::MDL::MDL> cachedRespawnMDL;
	std::unique_ptr<mdlpp::VTX::VTX> cachedRespawnVTX;
	std::unique_ptr<mdlpp::VVD::VVD> cachedRespawnVVD;
	std::vector<int> respawnBodygroupSelection; // per bodypart: selected model index
	bool updatingBodygroupsTab = false;
	bool respawnCameraInitialized = false;
};
