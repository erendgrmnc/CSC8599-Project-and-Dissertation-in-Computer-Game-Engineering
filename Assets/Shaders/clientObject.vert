#version 400 core

uniform mat4 modelMatrix;
uniform mat4 viewProjMatrix;

layout(location = 0) in vec3 position;
layout(location = 3) in vec3 normal;

out vec3 worldNormal;

void main() {
	gl_Position = viewProjMatrix * modelMatrix * vec4(position, 1.0);
	worldNormal = mat3(modelMatrix) * normal;
}
