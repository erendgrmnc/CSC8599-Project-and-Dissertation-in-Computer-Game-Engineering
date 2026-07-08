#version 400 core

uniform mat4 viewProjMatrix;

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 colour;

out vec4 passColour;

void main() {
	gl_Position = viewProjMatrix * vec4(position, 1.0);
	passColour = colour;
}
