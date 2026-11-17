#ifndef MDKVSCENE_H
#define MDKVSCENE_H

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <cmath>
#include <cstring>
#include <vector>
#include <map>

struct Vec2
{
	float x, y;

	Vec2() : x(0.0f), y(0.0f) {}
	Vec2(float fx, float fy) : x(fx), y(fy) {}
};

struct Vec3
{
	float x, y, z;

	Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
	Vec3(float fx, float fy, float fz) : x(fx), y(fy), z(fz) {}

	void normalize()
	{
		float len = sqrtf(x * x + y * y + z * z);
		if (len > 0.0f)
		{
			x /= len;
			y /= len;
			z /= len;
		}
	}
};

struct PackedVertex
{
	Vec3 position;
	Vec2 uv;
	Vec3 normal;

	bool operator<(const PackedVertex& that) const
	{
		return memcmp((void*)this, (void*)&that, sizeof(PackedVertex)) > 0;
	}
};

class CMdkvScene
{
public:
	CMdkvScene();
	virtual ~CMdkvScene();

	bool Init();
	void Draw(const glm::mat4& matProjection, const glm::mat4& matView);
	void Term();

	// リソース (TEXTFILE) からシェーダを読み込んでプログラムを作成する
	static GLuint CreateProgram(int nVertResID, int nFragResID);

private:
	static bool LoadShader(HINSTANCE hInst, int nResID, char** ppBuf);
	static GLuint CompileShader(GLenum nType, int nResID);

	GLuint m_programID;
	GLuint m_vertexbufferID;
	GLuint m_elementbufferID;
	GLuint m_textureID;
	GLuint m_VertexArrayID;
	GLsizei m_nIndexCount;
	GLint m_MatrixID;
	GLint m_ViewMatrixID;
	GLint m_ModelMatrixID;
	GLint m_LightID;
	GLint m_TextureSamplerID;
	glm::mat4 m_Model;
	glm::vec3 m_LightPos;

	bool LoadTexture(int nResID);
	bool LoadModel(int nResID);
	bool LoadBinary(HINSTANCE hInst, int nResID, char** ppBuf, unsigned int* pnSize);
	GLuint LoadTGAFromBuffer(char* buff, unsigned int nSize, unsigned char** ppbuf, unsigned short* psWidth, unsigned short* psHeight);
	int LoadObjFromBuffer(float fScale, char* buff, std::vector<unsigned int>& vertexIndices, std::vector<unsigned int>& uvIndices, std::vector<unsigned int>& normalIndices, std::vector<Vec3>& temp_vertices, std::vector<Vec2>& temp_uvs, std::vector<Vec3>& temp_normals, Vec3* pMax, Vec3* pMin);
	bool ExtractObjModel(std::vector<unsigned int>& vertexIndices, std::vector<unsigned int>& uvIndices, std::vector<unsigned int>& normalIndices, std::vector<Vec3>& temp_vertices, std::vector<Vec2>& temp_uvs, std::vector<Vec3>& temp_normals, std::vector<Vec3>& out_vertices, std::vector<Vec2>& out_uvs, std::vector<Vec3>& out_normals);
	bool ReIndexObjModel(std::vector<Vec3>& in_vertices, std::vector<Vec2>& in_uvs, std::vector<Vec3>& in_normals, std::vector<unsigned short>& out_indices, std::vector<float>& out_vertices);
	bool getSimilarVertexIndex_fast(PackedVertex& packed, std::map<PackedVertex, unsigned int>& VertexToOutIndex, unsigned int& result);
};

#endif
