#version 400 core

uniform sampler2D fontTex;

in vec4 passColour;
in vec2 passUV;
out vec4 fragColour;

void main() {
	// The PressStart2P atlas stores glyph coverage in the red channel.
	float alpha = texture(fontTex, passUV).r;
	if (alpha < 0.00001) {
		discard;
	}
	fragColour = passColour * vec4(1.0, 1.0, 1.0, alpha);
}
