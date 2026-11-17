#ifndef MDKVVIEW_H
#define MDKVVIEW_H

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <vector>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

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
	enum EEye
	{
		Eye_Left = 0,
		Eye_Right = 1,
		Eye_Count = 2,
	};

	struct FramebufferDesc
	{
		GLuint m_nDepthBufferId;
		GLuint m_nRenderTextureId;
		GLuint m_nRenderFramebufferId;
		GLuint m_nResolveTextureId;
		GLuint m_nResolveFramebufferId;
		GLuint m_nSwapchainFramebufferId;		// swapchain image を貼り付けて blit する先
		XrSwapchain m_swapchain;
		std::vector<XrSwapchainImageOpenGLKHR> m_swapchainImages;
	};

	bool InitGL();
	void TermGL();

	// XR
	bool InitVR();
	void TermVR();
	void ProcessVREvents();
	bool UpdateHMDMatrixPose(XrTime predictedDisplayTime);
	glm::mat4 GetHMDMatrixProjectionEye(EEye nEye);
	glm::mat4 GetHMDMatrixPoseEye(EEye nEye);
	glm::mat4 GetCurrentViewMatrix(EEye nEye);
	static glm::mat4 ConvertXrPose(const XrPosef& pose);
	bool ShowXrError(XrResult result, const char* pszFunction);

	// Render targets
	bool SetupStereoRenderTargets();
	bool CreateFrameBuffer(int nWidth, int nHeight, FramebufferDesc& desc);
	void DeleteFrameBuffer(FramebufferDesc& desc);
	bool SetupCompanionWindow();
	bool RenderStereoTargets(std::vector<XrCompositionLayerProjectionView>& projectionViews);
	bool RenderEye(EEye nEye, FramebufferDesc& desc);
	void RenderCompanionWindow();

	HWND m_hWnd;
	HDC m_hDC;
	HGLRC m_hRC;
	CMdkvScene* m_pScene;

	int m_nWidth;	// companion window client size
	int m_nHeight;

	// OpenXR
	XrInstance m_instance;
	XrSystemId m_systemId;
	XrSession m_session;
	XrSpace m_appSpace;
	XrViewConfigurationType m_viewConfigType;
	XrEnvironmentBlendMode m_blendMode;
	XrSessionState m_sessionState;
	bool m_bSessionRunning;
	int64_t m_nColorSwapchainFormat;
	std::vector<XrViewConfigurationView> m_configViews;
	std::vector<XrView> m_views;

	glm::mat4 m_mat4EyePosLeft;		// view matrix (左目)
	glm::mat4 m_mat4EyePosRight;	// view matrix (右目)
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
