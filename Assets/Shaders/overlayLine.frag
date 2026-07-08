#version 400 core

in vec4 passColour;
out vec4 fragColour;

void main() {
	fragColour = passColour;
}
