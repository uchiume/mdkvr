#ifndef MDKVVIEW_H
#define MDKVVIEW_H

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <openvr.h>

extern HINSTANCE g_hInst;

class CMdkvScene;

class CMdkvView
{
public:
	CMdkvView();
	virtual ~CMdkvView();
	bool Init(int nWidth, int nHeight);
	void Draw();
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
	struct FramebufferDesc
	{
		GLuint m_nDepthBufferId;
		GLuint m_nRenderTextureId;
		GLuint m_nRenderFramebufferId;
		GLuint m_nResolveTextureId;
		GLuint m_nResolveFramebufferId;
	};

	bool InitGL();
	void TermGL();

	// VR
	bool InitVR();
	void TermVR();
	void ProcessVREvents();
	void UpdateHMDMatrixPose();
	glm::mat4 GetHMDMatrixProjectionEye(vr::Hmd_Eye nEye);
	glm::mat4 GetHMDMatrixPoseEye(vr::Hmd_Eye nEye);
	glm::mat4 GetCurrentViewMatrix(vr::Hmd_Eye nEye);
	static glm::mat4 ConvertSteamVRMatrix(const vr::HmdMatrix34_t& matPose);
	static glm::mat4 ConvertSteamVRMatrix(const vr::HmdMatrix44_t& mat);

	// Render targets
	bool SetupStereoRenderTargets();
	bool CreateFrameBuffer(int nWidth, int nHeight, FramebufferDesc& desc);
	void DeleteFrameBuffer(FramebufferDesc& desc);
	bool SetupCompanionWindow();
	void RenderStereoTargets();
	void RenderEye(vr::Hmd_Eye nEye, FramebufferDesc& desc);
	void RenderCompanionWindow();

	HWND m_hWnd;
	HDC m_hDC;
	HGLRC m_hRC;
	CMdkvScene* m_pScene;

	int m_nWidth;	// companion window client size
	int m_nHeight;

	vr::IVRSystem* m_pHMD;
	vr::TrackedDevicePose_t m_rTrackedDevicePose[vr::k_unMaxTrackedDeviceCount];
	glm::mat4 m_mat4HMDPose;
	glm::mat4 m_mat4EyePosLeft;
	glm::mat4 m_mat4EyePosRight;
	glm::mat4 m_mat4ProjectionLeft;
	glm::mat4 m_mat4ProjectionRight;
	float m_fNearClip;
	float m_fFarClip;

	uint32_t m_nRenderWidth;
	uint32_t m_nRenderHeight;
	FramebufferDesc m_leftEyeDesc;
	FramebufferDesc m_rightEyeDesc;

	GLuint m_unCompanionProgramID;
	GLuint m_unCompanionVAO;
	GLuint m_glCompanionVertBuffer;
	GLuint m_glCompanionIndexBuffer;
	GLsizei m_nCompanionIndexSize;
};

#endif
