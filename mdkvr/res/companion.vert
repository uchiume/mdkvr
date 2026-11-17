#version 330 core

// Companion window: draws the resolved eye textures to the desktop window.
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 v2UVIn;

noperspective out vec2 v2UV;

void main()
{
	v2UV = v2UVIn;
	gl_Position = vec4(position, 0.0, 1.0);
}
