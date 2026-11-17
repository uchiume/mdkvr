#include "pch.h"
#include "mdkvview.h"
HINSTANCE g_hInst;


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	CMdkvView viewer;
	//viewer.SetFileName(g_szFile);
	//viewer.SetPassword(g_szPass);
	//viewer.SetMode(g_szMode);

	bool bRet = viewer.Init(1124, 630);
	if (bRet != true)
	{
		return -1;
	}

	MSG msg;

	do
	{
		if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			viewer.Draw();
		}

	} while (msg.message != WM_QUIT);

	return 0;
}

int _tmain(int argc, _TCHAR *argv[])
{
	g_hInst = ::GetModuleHandle(NULL);

	int ret = WinMain(g_hInst, NULL, NULL, 1);

	return ret;
}
