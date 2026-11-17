#include "pch.h"
#include "mdkvscene.h"
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <string>
#include <sstream>
#include <cfloat>

#include "resource.h"

extern HINSTANCE g_hInst;

#pragma pack(push, 1)
struct TargaHeader
{
	unsigned char idLength;
	unsigned char colorMapType;
	unsigned char imageType;
	unsigned short colorMapOrigin;
	unsigned short colorMapLength;
	unsigned char colorMapDepth;
	unsigned short xOrigin;
	unsigned short yOrigin;
	unsigned short width;
	unsigned short height;
	unsigned char bpp;
	unsigned char descriptor;
};
#pragma pack(pop)

CMdkvScene::CMdkvScene()
	: m_programID(0)
	, m_vertexbufferID(0)
	, m_elementbufferID(0)
	, m_textureID(0)
	, m_VertexArrayID(0)
	, m_nIndexCount(0)
	, m_MatrixID(-1)
	, m_ViewMatrixID(-1)
	, m_ModelMatrixID(-1)
	, m_LightID(-1)
	, m_TextureSamplerID(-1)
	, m_Model(1.0f)
	, m_LightPos(0.0f)
{
}

CMdkvScene::~CMdkvScene()
{
}

bool CMdkvScene::Init()
{
	glGenVertexArrays(1, &m_VertexArrayID);
	glBindVertexArray(m_VertexArrayID);

	m_programID = CreateProgram(IDR_SCENE_VSHADER, IDR_SCENE_FSHADER);
	if (m_programID == 0)
	{
		return false;
	}

	// Get a handle for our uniforms
	m_MatrixID = glGetUniformLocation(m_programID, "MVP");
	m_ViewMatrixID = glGetUniformLocation(m_programID, "V");
	m_ModelMatrixID = glGetUniformLocation(m_programID, "M");
	m_LightID = glGetUniformLocation(m_programID, "LightPosition_worldspace");
	m_TextureSamplerID = glGetUniformLocation(m_programID, "myTextureSampler");

	// Model matrix : トラッキング空間 (床が y=0, 単位 m) で、正面 1.5m・目線の高さに 0.6m 角のキューブを置く
	// (cube.txt は ±1 の大きさ。一様スケールなのでシェーダの法線計算はそのままで良い)
	m_Model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.2f, -1.5f))
		* glm::scale(glm::mat4(1.0f), glm::vec3(0.3f));

	// ライト位置 (ワールド座標)
	m_LightPos = glm::vec3(4.0f, 5.0f, 3.0f);

	if (!LoadTexture(IDR_CUBE_TGA))
	{
		return false;
	}

	if (!LoadModel(IDR_CUBE_OBJ))
	{
		return false;
	}

	return true;
}

bool CMdkvScene::LoadTexture(int nResID)
{
	char* pData = NULL;
	unsigned int nSize = 0;
	if (!LoadBinary(g_hInst, nResID, &pData, &nSize))
	{
		return false;
	}

	unsigned char* pImage = NULL;
	unsigned short sWidth = 0;
	unsigned short sHeight = 0;
	GLuint nImageSize = LoadTGAFromBuffer(pData, nSize, &pImage, &sWidth, &sHeight);
	delete[] pData;
	if (nImageSize == 0)
	{
		return false;
	}

	glGenTextures(1, &m_textureID);
	glBindTexture(GL_TEXTURE_2D, m_textureID);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sWidth, sHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, pImage);
	delete[] pImage;

	// LoadObjFromBuffer は V 座標を反転するので、繰り返しで 0..1 に折り返す
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glGenerateMipmap(GL_TEXTURE_2D);

	return true;
}

bool CMdkvScene::LoadModel(int nResID)
{
	char* pszObj = NULL;
	if (!LoadShader(g_hInst, nResID, &pszObj))
	{
		return false;
	}

	std::vector<unsigned int> vertexIndices, uvIndices, normalIndices;
	std::vector<Vec3> temp_vertices;
	std::vector<Vec2> temp_uvs;
	std::vector<Vec3> temp_normals;
	Vec3 vMax, vMin;
	int nMode = LoadObjFromBuffer(1.0f, pszObj, vertexIndices, uvIndices, normalIndices, temp_vertices, temp_uvs, temp_normals, &vMax, &vMin);
	delete[] pszObj;
	if (nMode == 0)
	{
		return false;
	}

	std::vector<Vec3> vertices;
	std::vector<Vec2> uvs;
	std::vector<Vec3> normals;
	if (!ExtractObjModel(vertexIndices, uvIndices, normalIndices, temp_vertices, temp_uvs, temp_normals, vertices, uvs, normals))
	{
		return false;
	}
	// 同じ頂点をまとめて、頂点 (位置, 法線, UV) とインデックスに分ける
	std::vector<unsigned short> indices;
	std::vector<float> indexed_vertices;
	if (!ReIndexObjModel(vertices, uvs, normals, indices, indexed_vertices))
	{
		return false;
	}
	m_nIndexCount = (GLsizei)indices.size();

	glGenBuffers(1, &m_vertexbufferID);
	glBindBuffer(GL_ARRAY_BUFFER, m_vertexbufferID);
	glBufferData(GL_ARRAY_BUFFER, indexed_vertices.size() * sizeof(float), &indexed_vertices[0], GL_STATIC_DRAW);

	glGenBuffers(1, &m_elementbufferID);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_elementbufferID);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned short), &indices[0], GL_STATIC_DRAW);

	return true;
}

bool CMdkvScene::LoadShader(HINSTANCE hInst, int nResID, char** ppBuf)
{
	HRSRC hRes = ::FindResourceA(hInst, MAKEINTRESOURCEA(nResID), "TEXTFILE");
	if (hRes == NULL)
	{
		return false;
	}
	DWORD dwSize = ::SizeofResource(hInst, hRes);
	if (dwSize == 0)
	{
		return false;
	}
	HGLOBAL hMem = ::LoadResource(hInst, hRes);
	if (hMem == NULL)
	{
		return false;
	}
	LPVOID pMem = ::LockResource(hMem);
	if (pMem == NULL)
	{
		::FreeResource(hMem);
		return false;
	}

	char* pBuf = new char[dwSize + 1];

	if (pBuf == NULL)
	{
		::FreeResource(hMem);
		return false;
	}

	strncpy_s(pBuf, dwSize + 1, (char*)pMem, (size_t)dwSize);

	pBuf[dwSize] = '\0';
	*ppBuf = pBuf;

	::FreeResource(hMem);

	return true;
}

bool CMdkvScene::LoadBinary(HINSTANCE hInst, int nResID, char** ppBuf, unsigned int* pnSize)
{
	HRSRC hRes = ::FindResource(hInst, MAKEINTRESOURCE(nResID), RT_RCDATA);
	if (hRes == NULL)
	{
		return false;
	}
	DWORD dwSize = ::SizeofResource(hInst, hRes);
	if (dwSize == 0)
	{
		return false;
	}
	HGLOBAL hMem = ::LoadResource(hInst, hRes);
	if (hMem == NULL)
	{
		return false;
	}
	LPVOID pMem = ::LockResource(hMem);
	if (pMem == NULL)
	{
		return false;
	}

	char* pBuf = new char[dwSize];
	memcpy_s(pBuf, dwSize, pMem, dwSize);
	*ppBuf = pBuf;
	*pnSize = dwSize;

	return true;
}

GLuint CMdkvScene::CompileShader(GLenum nType, int nResID)
{
	char* pszSource = NULL;
	if (!LoadShader(g_hInst, nResID, &pszSource))
	{
		return 0;
	}

	GLuint shaderID = glCreateShader(nType);
	const char* pszSourcePointer = pszSource;
	glShaderSource(shaderID, 1, &pszSourcePointer, NULL);
	glCompileShader(shaderID);
	delete[] pszSource;

	GLint nResult = GL_FALSE;
	glGetShaderiv(shaderID, GL_COMPILE_STATUS, &nResult);
	if (nResult != GL_TRUE)
	{
		GLint nLogLength = 0;
		glGetShaderiv(shaderID, GL_INFO_LOG_LENGTH, &nLogLength);
		if (nLogLength > 0)
		{
			std::vector<char> log(nLogLength + 1);
			glGetShaderInfoLog(shaderID, nLogLength, NULL, &log[0]);
			::OutputDebugStringA(&log[0]);
		}
		glDeleteShader(shaderID);
		return 0;
	}

	return shaderID;
}

GLuint CMdkvScene::CreateProgram(int nVertResID, int nFragResID)
{
	GLuint vertShaderID = CompileShader(GL_VERTEX_SHADER, nVertResID);
	GLuint fragShaderID = CompileShader(GL_FRAGMENT_SHADER, nFragResID);
	if (vertShaderID == 0 || fragShaderID == 0)
	{
		glDeleteShader(vertShaderID);
		glDeleteShader(fragShaderID);
		return 0;
	}

	GLuint programID = glCreateProgram();
	glAttachShader(programID, vertShaderID);
	glAttachShader(programID, fragShaderID);
	glLinkProgram(programID);

	glDetachShader(programID, vertShaderID);
	glDetachShader(programID, fragShaderID);
	glDeleteShader(vertShaderID);
	glDeleteShader(fragShaderID);

	GLint nResult = GL_FALSE;
	glGetProgramiv(programID, GL_LINK_STATUS, &nResult);
	if (nResult != GL_TRUE)
	{
		GLint nLogLength = 0;
		glGetProgramiv(programID, GL_INFO_LOG_LENGTH, &nLogLength);
		if (nLogLength > 0)
		{
			std::vector<char> log(nLogLength + 1);
			glGetProgramInfoLog(programID, nLogLength, NULL, &log[0]);
			::OutputDebugStringA(&log[0]);
		}
		glDeleteProgram(programID);
		return 0;
	}

	return programID;
}

GLuint CMdkvScene::LoadTGAFromBuffer(char* buff, unsigned int nSize, unsigned char** ppbuf, unsigned short* psWidth, unsigned short* psHeight)
{
	int width, height, bpp, imageSize;
	unsigned char* targaImage;

	if (nSize < sizeof(TargaHeader))
	{
		return 0;
	}
	TargaHeader header;
	memcpy_s(&header, 18, buff, 18);
	width = (int)header.width;
	height = (int)header.height;
	imageSize = width * height * 4;
	bpp = header.bpp;
	if (bpp != 32)
	{
		return 0;
	}

	if (header.imageType != 2 && header.imageType != 10)
	{
		return 0;
	}
	char* pPos = buff + 18 + header.idLength;
	char* pEnd = buff + nSize;
	if (header.imageType == 2 && pEnd - pPos < imageSize)
	{
		return 0;
	}
	targaImage = new unsigned char[imageSize];
	if (!targaImage)
	{
		return 0;
	}
	int bytePerPixel = bpp / 8;
	if (header.imageType == 2)
	{
		errno_t err = memcpy_s(targaImage, imageSize, pPos, imageSize);
		if (err != 0) {
			delete[] targaImage;
			return 0;
		}
	}
	else
	{
		// RLE compressed
		int nPixel = 0;
		int nPixelCount = width * height;
		while (nPixel < nPixelCount)
		{
			if (pPos >= pEnd)
			{
				delete[] targaImage;
				return 0;
			}
			unsigned char packet = (unsigned char)*pPos++;
			int count = (packet & 0x7F) + 1;
			bool bRepeat = (packet & 0x80) != 0;
			int nDataSize = bRepeat ? bytePerPixel : count * bytePerPixel;
			if (nPixel + count > nPixelCount || pEnd - pPos < nDataSize)
			{
				delete[] targaImage;
				return 0;
			}
			for (int i = 0; i < count; i++)
			{
				memcpy(targaImage + (nPixel + i) * bytePerPixel, bRepeat ? pPos : pPos + i * bytePerPixel, bytePerPixel);
			}
			pPos += nDataSize;
			nPixel += count;
		}
	}
	unsigned int temp;
	// swap BGR(A) to RGB(A)
	for (int i = 0; i < imageSize; i += bytePerPixel) {
		temp = targaImage[i];
		targaImage[i] = targaImage[i + 2];
		targaImage[i + 2] = temp;
	}
	*ppbuf = targaImage;
	*psWidth = width;
	*psHeight = height;

	return imageSize;
}

int CMdkvScene::LoadObjFromBuffer(float fScale, char* buff, std::vector<unsigned int>& vertexIndices, std::vector<unsigned int>& uvIndices, std::vector<unsigned int>& normalIndices, std::vector<Vec3>& temp_vertices, std::vector<Vec2>& temp_uvs, std::vector<Vec3>& temp_normals, Vec3* pMax, Vec3* pMin)
{
	int nMode = 0;	// auto detect
	float fXmax, fXmin, fYmax, fYmin, fZmax, fZmin;
	fXmax = -FLT_MAX; fXmin = FLT_MAX; fYmax = -FLT_MAX; fYmin = FLT_MAX; fZmax = -FLT_MAX; fZmin = FLT_MAX;

	std::string str(buff);
	std::istringstream stream(str);
	std::string field;
	char delimiter = '\n';

	while (std::getline(stream, field, delimiter))
	{
		if (field.substr(0, 2) == "v ")
		{
			Vec3 vertex;
			int nPos = (int)field.find(' ', 0);
			std::string val = field.substr(nPos + 1);
			int res = sscanf_s(val.c_str(), "%f %f %f", &vertex.x, &vertex.y, &vertex.z);
			if (res != 3) return 0;

			vertex.x = vertex.x * fScale;
			vertex.y = vertex.y * fScale;
			vertex.z = vertex.z * fScale;

			if (vertex.x > fXmax) fXmax = vertex.x;
			if (vertex.x < fXmin) fXmin = vertex.x;
			if (vertex.y > fYmax) fYmax = vertex.y;
			if (vertex.y < fYmin) fYmin = vertex.y;
			if (vertex.z > fZmax) fZmax = vertex.z;
			if (vertex.z < fZmin) fZmin = vertex.z;
			temp_vertices.push_back(vertex);
		}
		else if (field.substr(0, 3) == "vt ")
		{
			Vec2 uv;
			int nPos = (int)field.find(' ', 0);
			std::string val = field.substr(nPos + 1);
			int res = sscanf_s(val.c_str(), "%f %f", &uv.x, &uv.y);
			if (res != 2) return 0;

			uv.y = -uv.y;
			temp_uvs.push_back(uv);
		}
		else if (field.substr(0, 3) == "vn ")
		{
			Vec3 normal;
			int nPos = (int)field.find(' ', 0);
			std::string val = field.substr(nPos + 1);
			int res = sscanf_s(val.c_str(), "%f %f %f", &normal.x, &normal.y, &normal.z);
			if (res != 3) return 0;

			temp_normals.push_back(normal);
		}
		else if (field.substr(0, 2) == "f ")
		{
			unsigned int vertexIndex[4], uvIndex[4], normalIndex[4];
			int nPos = (int)field.find(' ', 0);
			std::string val = field.substr(nPos + 1);
			int nCount = 0;

			if (nMode == 0 || nMode == 1)
			{
				int res = sscanf_s(val.c_str(), "%u/%u/%u %u/%u/%u %u/%u/%u %u/%u/%u", &vertexIndex[0], &uvIndex[0], &normalIndex[0], &vertexIndex[1], &uvIndex[1], &normalIndex[1], &vertexIndex[2], &uvIndex[2], &normalIndex[2], &vertexIndex[3], &uvIndex[3], &normalIndex[3]);
				if (res == 9 || res == 12)
				{
					nMode = 1;
					nCount = res / 3;
				}
			}
			if (nCount == 0 && (nMode == 0 || nMode == 2))
			{
				int res = sscanf_s(val.c_str(), "%u//%u %u//%u %u//%u %u//%u", &vertexIndex[0], &normalIndex[0], &vertexIndex[1], &normalIndex[1], &vertexIndex[2], &normalIndex[2], &vertexIndex[3], &normalIndex[3]);
				if (res == 6 || res == 8)
				{
					nMode = 2;
					nCount = res / 2;
				}
			}
			if (nCount == 0 && (nMode == 0 || nMode == 3))
			{
				int res = sscanf_s(val.c_str(), "%u/%u %u/%u %u/%u %u/%u", &vertexIndex[0], &uvIndex[0], &vertexIndex[1], &uvIndex[1], &vertexIndex[2], &uvIndex[2], &vertexIndex[3], &uvIndex[3]);
				if (res == 6 || res == 8)
				{
					nMode = 3;
					nCount = res / 2;
				}
			}
			if (nCount == 0)
			{
				return 0;
			}

			static const int triangles[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
			for (int t = 0; t < nCount - 2; t++)
			{
				for (int k = 0; k < 3; k++)
				{
					int i = triangles[t][k];
					vertexIndices.push_back(vertexIndex[i] - 1);
					if (nMode != 2)
					{
						uvIndices.push_back(uvIndex[i] - 1);
					}
					if (nMode != 3)
					{
						normalIndices.push_back(normalIndex[i] - 1);
					}
				}
			}
		}
	}

	pMax->x = fXmax;
	pMax->y = fYmax;
	pMax->z = fZmax;
	pMin->x = fXmin;
	pMin->y = fYmin;
	pMin->z = fZmin;

	return nMode;
}

bool CMdkvScene::ExtractObjModel(std::vector<unsigned int>& vertexIndices, std::vector<unsigned int>& uvIndices, std::vector<unsigned int>& normalIndices, std::vector<Vec3>& temp_vertices, std::vector<Vec2>& temp_uvs, std::vector<Vec3>& temp_normals, std::vector<Vec3>& out_vertices, std::vector<Vec2>& out_uvs, std::vector<Vec3>& out_normals)
{
	if (vertexIndices.size() == 0)
		return false;

	if (uvIndices.size() == 0)
	{
		for (unsigned int i = 0; i < vertexIndices.size(); i++) {
			unsigned int vertexIndex = vertexIndices[i];
			unsigned int normalIndex = normalIndices[i];

			Vec3 vertex = temp_vertices[vertexIndex];
			Vec2 uv = Vec2(0, 0);
			Vec3 normal = temp_normals[normalIndex];
			normal.normalize();

			out_vertices.push_back(vertex);
			out_uvs.push_back(uv);
			out_normals.push_back(normal);
		}
	}
	else if (normalIndices.size() == 0)
	{
		for (unsigned int i = 0; i < vertexIndices.size(); i++) {
			unsigned int vertexIndex = vertexIndices[i];
			unsigned int uvIndex = uvIndices[i];

			Vec3 vertex = temp_vertices[vertexIndex];
			Vec2 uv = temp_uvs[uvIndex];
			Vec3 normal = Vec3(0, 0, 0);

			out_vertices.push_back(vertex);
			out_uvs.push_back(uv);
			out_normals.push_back(normal);
		}
	}
	else
	{
		for (unsigned int i = 0; i < vertexIndices.size(); i++) {
			unsigned int vertexIndex = vertexIndices[i];
			unsigned int uvIndex = uvIndices[i];
			unsigned int normalIndex = normalIndices[i];

			Vec3 vertex = temp_vertices[vertexIndex];
			Vec2 uv = temp_uvs[uvIndex];
			Vec3 normal = temp_normals[normalIndex];

			out_vertices.push_back(vertex);
			out_uvs.push_back(uv);
			out_normals.push_back(normal);
		}
	}

	return true;
}

bool CMdkvScene::ReIndexObjModel(std::vector<Vec3>& in_vertices,
	std::vector<Vec2>& in_uvs,
	std::vector<Vec3>& in_normals,
	std::vector<unsigned short>& out_indices,
	std::vector<float>& out_vertices)
{
	std::map<PackedVertex, unsigned int> VertexToOutIndex;

	// For each input vertex
	for (unsigned int i = 0; i < in_vertices.size(); i++) {

		PackedVertex packed = { in_vertices[i], in_uvs[i], in_normals[i] };

		// Try to find a similar vertex in out_XXXX
		unsigned int index;
		bool found = getSimilarVertexIndex_fast(packed, VertexToOutIndex, index);

		if (found) { // A similar vertex is already in the VBO, use it instead !
			out_indices.push_back((unsigned short)index);
		}
		else { // If not, it needs to be added in the output data.
			out_vertices.push_back(in_vertices[i].x);
			out_vertices.push_back(in_vertices[i].y);
			out_vertices.push_back(in_vertices[i].z);
			out_vertices.push_back(in_normals[i].x);
			out_vertices.push_back(in_normals[i].y);
			out_vertices.push_back(in_normals[i].z);
			out_vertices.push_back(in_uvs[i].x);
			out_vertices.push_back(in_uvs[i].y);

			unsigned int newindex = (unsigned int)out_vertices.size() / 8 - 1;
			out_indices.push_back((unsigned short)newindex);
			VertexToOutIndex[packed] = newindex;
		}
	}

	return true;
}

bool CMdkvScene::getSimilarVertexIndex_fast(PackedVertex& packed, std::map<PackedVertex, unsigned int>& VertexToOutIndex, unsigned int& result)
{
	std::map<PackedVertex, unsigned int>::iterator it = VertexToOutIndex.find(packed);
	if (it == VertexToOutIndex.end()) {
		return false;
	}
	else {
		result = it->second;
		return true;
	}
}

void CMdkvScene::Draw(const glm::mat4& matProjection, const glm::mat4& matView)
{
	// Use our shader
	glUseProgram(m_programID);

	// 目ごとの射影 * ビュー (目の位置 * HMD 姿勢) * モデル
	glm::mat4 matMVP = matProjection * matView * m_Model;
	glUniformMatrix4fv(m_MatrixID, 1, GL_FALSE, &matMVP[0][0]);
	glUniformMatrix4fv(m_ModelMatrixID, 1, GL_FALSE, &m_Model[0][0]);
	glUniformMatrix4fv(m_ViewMatrixID, 1, GL_FALSE, &matView[0][0]);

	glUniform3f(m_LightID, m_LightPos.x, m_LightPos.y, m_LightPos.z);

	// Bind our texture in Texture Unit 0
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_textureID);
	// Set our "myTextureSampler" sampler to use Texture Unit 0
	glUniform1i(m_TextureSamplerID, 0);

	glBindVertexArray(m_VertexArrayID);

	// 頂点バッファは 位置(3), 法線(3), UV(2) の順に並んでいる
	const GLsizei nStride = 8 * sizeof(float);
	glBindBuffer(GL_ARRAY_BUFFER, m_vertexbufferID);

	// 1st attribute buffer : vertices
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, nStride, (void*)0);
	// 2nd attribute buffer : UVs
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, nStride, (void*)(6 * sizeof(float)));
	// 3rd attribute buffer : normals
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, nStride, (void*)(3 * sizeof(float)));

	// Index buffer
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_elementbufferID);

	glDrawElements(GL_TRIANGLES, m_nIndexCount, GL_UNSIGNED_SHORT, (void*)0);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);

	glBindVertexArray(0);
	glUseProgram(0);
}

void CMdkvScene::Term()
{
	glDeleteBuffers(1, &m_vertexbufferID);
	glDeleteBuffers(1, &m_elementbufferID);
	glDeleteTextures(1, &m_textureID);
	glDeleteVertexArrays(1, &m_VertexArrayID);
	glDeleteProgram(m_programID);

	m_vertexbufferID = 0;
	m_elementbufferID = 0;
	m_textureID = 0;
	m_VertexArrayID = 0;
	m_programID = 0;
}
