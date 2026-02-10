#version 330 core

layout(location = 0) in vec3 vPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;

uniform mat4 uMVP;
uniform mat4 uMV;
uniform mat3 uNormalMatrix;
uniform vec3 uEyePosition;

out vec3 fNormal;
out float fDepth;
out vec2 fUVMesh;
out vec2 fUVMatCap;

void main() {
    vec4 position = uMVP * vec4(vPos, 1.0);
    gl_Position = position;
    fNormal = vNormal;
    fUVMesh = vUV;

    // https://www.clicktorelease.com/blog/creating-spherical-environment-mapping-shader

    // View vector in view-space (camera is at origin in view-space).
    // The old implementation used uEyePosition to "fake" a direction, but that breaks when uMV is a real lookAt()
    // (eye transforms to the origin). Using the correct view-vector keeps matcap lighting stable.
    vec3 e = normalize(-vec3(uMV * vec4(vPos, 1.0)));
    vec3 n = normalize(uNormalMatrix * vNormal);
    vec3 r = reflect(e, n);
    r.z += 1.0;
    float m = 2.0 * length(r);
    fUVMatCap = r.xy / m + 0.5;

    // Unused right now
    fDepth = position.w;
}
