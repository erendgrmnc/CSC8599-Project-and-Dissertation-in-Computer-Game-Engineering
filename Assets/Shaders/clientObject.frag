#version 400 core

uniform vec4 objectColour;

in vec3 worldNormal;
out vec4 fragColour;

void main() {
	// Guard against missing/zero normals so normalize() can't produce NaNs.
	float len = length(worldNormal);
	vec3 n = (len > 0.0001) ? worldNormal / len : vec3(0.0, 1.0, 0.0);

	vec3 lightDir = normalize(vec3(0.4, 0.9, 0.35));
	float diffuse = max(dot(n, lightDir), 0.0);
	float shade = 0.35 + 0.65 * diffuse;

	fragColour = vec4(objectColour.rgb * shade, 1.0);
}
