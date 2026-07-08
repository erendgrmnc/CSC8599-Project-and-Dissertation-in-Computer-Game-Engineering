#version 400 core

uniform mat4 orthProj;

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 colour;
layout(location = 2) in vec2 texCoord;

out vec4 passColour;
out vec2 passUV;

void main() {
	gl_Position = orthProj * vec4(position, 1.0);
	passColour = colour;
	passUV = texCoord;
}
