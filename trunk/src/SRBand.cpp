#include "stdafx.h"
#include "SRBand.h"
#include "Guid.h"
#include <strsafe.h>
#include "resource.h"
#include "Registry.h"


std::map<DWORD, CDeskBand*> CDeskBand::m_desklist;	///< set of CDeskBand objects which use the keyboard hook

CDeskBand::CDeskBand() : m_bFocus(false)
	, m_hwndParent(NULL)
	, m_hWnd(NULL)
	, m_hWndEdit(NULL)
	, m_dwViewMode(0)
	, m_dwBandID(0)
	, m_oldEditWndProc(NULL)
	, m_pSite(NULL)
	, m_bHideUnchanged(false)
{
	m_ObjRefCount = 1;
	g_DllRefCount++;

	m_tbSize.cx = 0;
	m_tbSize.cy = 0;

	INITCOMMONCONTROLSEX used = {
		sizeof(INITCOMMONCONTROLSEX),
		ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_COOL_CLASSES
	};
	InitCommonControlsEx(&used);
}

CDeskBand::~CDeskBand()
{
	// This should have been freed in a call to SetSite(NULL), but 
	// it is defined here to be safe.
	if (m_pSite)
	{
		UnhookWindowsHookEx(m_hook);
		m_pSite->Release();
		m_pSite = NULL;
		std::map<DWORD, CDeskBand*>::iterator it = m_desklist.find(GetCurrentThreadId());
		if (it != m_desklist.end())
			m_desklist.erase(it);
	}

	g_DllRefCount--;
}

//////////////////////////////////////////////////////////////////////////
// IUnknown methods
//////////////////////////////////////////////////////////////////////////
STDMETHODIMP CDeskBand::QueryInterface(REFIID riid, LPVOID *ppReturn)
{
	*ppReturn = NULL;

	// IUnknown
	if (IsEqualIID(riid, IID_IUnknown))
	{
		*ppReturn = this;
	}

	// IOleWindow
	else if (IsEqualIID(riid, IID_IOleWindow))
	{
		*ppReturn = (IOleWindow*)this;
	}

	// IDockingWindow
	else if (IsEqualIID(riid, IID_IDockingWindow))
	{
		*ppReturn = (IDockingWindow*)this;
	}   

	// IInputObject
	else if (IsEqualIID(riid, IID_IInputObject))
	{
		*ppReturn = (IInputObject*)this;
	}   

	// IObjectWithSite
	else if (IsEqualIID(riid, IID_IObjectWithSite))
	{
		*ppReturn = (IObjectWithSite*)this;
	}   

	// IDeskBand
	else if (IsEqualIID(riid, IID_IDeskBand))
	{
		*ppReturn = (IDeskBand*)this;
	}   

	// IPersist
	else if (IsEqualIID(riid, IID_IPersist))
	{
		*ppReturn = (IPersist*)this;
	}   

	// IPersistStream
	else if (IsEqualIID(riid, IID_IPersistStream))
	{
		*ppReturn = (IPersistStream*)this;
	}   

	if (*ppReturn)
	{
		(*(LPUNKNOWN*)ppReturn)->AddRef();
		return S_OK;
	}

	return E_NOINTERFACE;
}                                             

STDMETHODIMP_(DWORD) CDeskBand::AddRef()
{
	return ++m_ObjRefCount;
}


STDMETHODIMP_(DWORD) CDeskBand::Release()
{
	if (--m_ObjRefCount == 0)
	{
		delete this;
		return 0;
	}

	return m_ObjRefCount;
}

//////////////////////////////////////////////////////////////////////////
// IOleWindow methods
//////////////////////////////////////////////////////////////////////////
STDMETHODIMP CDeskBand::GetWindow(HWND *phWnd)
{
	*phWnd = m_hWnd;

	return S_OK;
}

STDMETHODIMP CDeskBand::ContextSensitiveHelp(BOOL /*fEnterMode*/)
{
	return E_NOTIMPL;
}

//////////////////////////////////////////////////////////////////////////
// IDockingWindow methods
//////////////////////////////////////////////////////////////////////////
STDMETHODIMP CDeskBand::ShowDW(BOOL fShow)
{
	if (m_hWnd)
	{
		if (fShow)
		{
			// show our window
			ShowWindow(m_hWnd, SW_SHOW);
		}
		else
		{
			// hide our window
			ShowWindow(m_hWnd, SW_HIDE);
		}
	}

	return S_OK;
}

STDMETHODIMP CDeskBand::CloseDW(DWORD /*dwReserved*/)
{
	ShowDW(FALSE);

	if (IsWindow(m_hWnd))
	{
		ImageList_Destroy(m_hToolbarImgList);
		DestroyWindow(m_hWnd);
	}
	m_hWnd = NULL;

	return S_OK;
}

STDMETHODIMP CDeskBand::ResizeBorderDW(LPCRECT /*prcBorder*/, 
									   IUnknown* /*punkSite*/, 
									   BOOL /*fReserved*/)
{
	// This method is never called for Band Objects.
	return E_NOTIMPL;
}

//////////////////////////////////////////////////////////////////////////
// IInputObject methods
//////////////////////////////////////////////////////////////////////////
STDMETHODIMP CDeskBand::UIActivateIO(BOOL fActivate, LPMSG /*pMsg*/)
{
	if (fActivate)
		SetFocus(m_hWnd);

	return S_OK;
}

STDMETHODIMP CDeskBand::HasFocusIO(void)
{
	// If this window or one of its descendants has the focus, return S_OK. 
	// Return S_FALSE if neither has the focus.
	if (m_bFocus)
		return S_OK;

	return S_FALSE;
}

STDMETHODIMP CDeskBand::TranslateAcceleratorIO(LPMSG pMsg)
{
	// we have to translate the accelerator keys ourselves, otherwise
	// the edit control won't get the keys the explorer uses itself 
	// (e.g. backspace)
	int nVirtKey = (int)(pMsg->wParam);
	if (VK_RETURN == nVirtKey)
	{
		// remove system beep on enter key by setting key code to 0
		pMsg->wParam = 0;
		::PostMessage(m_hWnd, WM_COMMAND, BM_CLICK, 1);
		return S_OK;
	}
	else if (WM_KEYDOWN == pMsg->message && nVirtKey == VK_TAB)
	{
		// loose the focus
		FocusChange(FALSE);
		return S_FALSE;
	}

	TranslateMessage(pMsg);
	DispatchMessage(pMsg);
	return S_OK;
}

//////////////////////////////////////////////////////////////////////////
// IObjectWithSite methods
//////////////////////////////////////////////////////////////////////////
STDMETHODIMP CDeskBand::SetSite(IUnknown* punkSite)
{
	// If a site is being held, release it.
	if (m_pSite)
	{
		UnhookWindowsHookEx(m_hook);
		m_pSite->Release();
		m_pSite = NULL;
		std::map<DWORD, CDeskBand*>::iterator it = m_desklist.find(GetCurrentThreadId());
		if (it != m_desklist.end())
			m_desklist.erase(it);
	}

	m_tbSize.cx = 0;
	m_tbSize.cy = 0;

	// If punkSite is not NULL, a new site is being set.
	if (punkSite)
	{
		// Get the parent window.
		IOleWindow  *pOleWindow;

		m_hwndParent = NULL;

		if (SUCCEEDED(punkSite->QueryInterface(IID_IOleWindow, 
			(LPVOID*)&pOleWindow)))
		{
			pOleWindow->GetWindow(&m_hwndParent);
			pOleWindow->Release();
		}

		if (!m_hwndParent)
			return E_FAIL;

		if (!RegisterAndCreateWindow())
			return E_FAIL;

		if (!BuildToolbarButtons())
			return E_FAIL;

		m_hook = SetWindowsHookEx(WH_KEYBOARD, KeyboardHookProc, NULL, GetCurrentThreadId());
		m_desklist[GetCurrentThreadId()] = this;

		// Get and keep the IInputObjectSite pointer.
		if (SUCCEEDED(punkSite->QueryInterface(IID_IInputObjectSite, 
			(LPVOID*)&m_pSite)))
		{
			return S_OK;
		}

		return E_FAIL;
	}

	return S_OK;
}

STDMETHODIMP CDeskBand::GetSite(REFIID riid, LPVOID *ppvReturn)
{
	*ppvReturn = NULL;

	if (m_pSite)
		return m_pSite->QueryInterface(riid, ppvReturn);

	return E_FAIL;
}

//////////////////////////////////////////////////////////////////////////
// IDeskBand methods
//////////////////////////////////////////////////////////////////////////
STDMETHODIMP CDeskBand::GetBandInfo(DWORD dwBandID, DWORD dwViewMode, DESKBANDINFO* pdbi)
{
	if (pdbi)
	{
		m_dwBandID = dwBandID;
		m_dwViewMode = dwViewMode;

		if (pdbi->dwMask & DBIM_MINSIZE)
		{
			if (DBIF_VIEWMODE_FLOATING & dwViewMode)
			{
				pdbi->ptMinSize.x = 200;
				pdbi->ptMinSize.y = 400;
			}
			else
			{
				pdbi->ptMinSize.x = MIN_SIZE_X;
				pdbi->ptMinSize.y = MIN_SIZE_Y;
			}
		}

		if (pdbi->dwMask & DBIM_MAXSIZE)
		{
			pdbi->ptMaxSize.x = -1;
			pdbi->ptMaxSize.y = 20;
		}

		if (pdbi->dwMask & DBIM_INTEGRAL)
		{
			pdbi->ptIntegral.x = 1;
			pdbi->ptIntegral.y = 1;
		}

		if (pdbi->dwMask & DBIM_ACTUAL)
		{
			pdbi->ptActual.x = 100;
			pdbi->ptActual.y = 20;
		}

		if (pdbi->dwMask & DBIM_TITLE)
		{
			StringCchCopy(pdbi->wszTitle, 256, L"StEx");
		}

		if (pdbi->dwMask & DBIM_MODEFLAGS)
		{
			pdbi->dwModeFlags = DBIMF_NORMAL;

			pdbi->dwModeFlags |= DBIMF_VARIABLEHEIGHT;
		}

		if (pdbi->dwMask & DBIM_BKCOLOR)
		{
			// Use the default background color by removing this flag.
			pdbi->dwMask &= ~DBIM_BKCOLOR;
		}

		return S_OK;
	}

	return E_INVALIDARG;
}

//////////////////////////////////////////////////////////////////////////
// IPersistStream methods
//////////////////////////////////////////////////////////////////////////
STDMETHODIMP CDeskBand::GetClassID(LPCLSID pClassID)
{
	// this is the only method of the IPersistStream interface we need
	*pClassID = CLSID_StExBand;

	return S_OK;
}

STDMETHODIMP CDeskBand::IsDirty(void)
{
	return S_FALSE;
}

STDMETHODIMP CDeskBand::Load(LPSTREAM /*pStream*/)
{
	return S_OK;
}

STDMETHODIMP CDeskBand::Save(LPSTREAM /*pStream*/, BOOL /*fClearDirty*/)
{
	return S_OK;
}

STDMETHODIMP CDeskBand::GetSizeMax(ULARGE_INTEGER * /*pul*/)
{
	return E_NOTIMPL;
}

//////////////////////////////////////////////////////////////////////////
// helper functions for our own DeskBand
//////////////////////////////////////////////////////////////////////////

LRESULT CALLBACK CDeskBand::WndProc(HWND hWnd, 
									UINT uMessage, 
									WPARAM wParam, 
									LPARAM lParam)
{
	CDeskBand *pThis = (CDeskBand*)GetWindowLongPtr(hWnd, GWL_USERDATA);

	switch (uMessage)
	{
	case WM_NCCREATE:
		{
			LPCREATESTRUCT lpcs = (LPCREATESTRUCT)lParam;
			pThis = (CDeskBand*)(lpcs->lpCreateParams);
			SetWindowLong(hWnd, GWL_USERDATA, (LONG)pThis);

			//set the window handle
			pThis->m_hWnd = hWnd;
		}
		break;

	case WM_COMMAND:
		return pThis->OnCommand(wParam, lParam);

	case WM_SETFOCUS:
		return pThis->OnSetFocus();

	case WM_KILLFOCUS:
		return pThis->OnKillFocus();

	case WM_SIZE:
		return pThis->OnSize(lParam);

	case WM_MOVE:
		return pThis->OnMove(lParam);

	}

	return DefWindowProc(hWnd, uMessage, wParam, lParam);
}

LRESULT CALLBACK CDeskBand::EditProc(HWND hWnd, UINT uMessage, WPARAM wParam, LPARAM lParam)
{
	CDeskBand *pThis = (CDeskBand*)GetWindowLongPtr(hWnd, GWL_USERDATA);
	if (uMessage == WM_SETFOCUS)
	{
		pThis->OnSetFocus();
	}
	return CallWindowProc(pThis->m_oldEditWndProc, hWnd, uMessage, wParam, lParam);
}

LRESULT CDeskBand::OnCommand(WPARAM wParam, LPARAM /*lParam*/)
{
	switch (HIWORD(wParam))
	{
	case BN_CLICKED:
		// button was pressed
		switch(LOWORD(wParam))
		{
		case 1:		// options
		case 2:		// cmd
		case 3:		// copy name
		case 4:		// copy path
		default:	// custom commands
			break;
		}
		FocusChange(false);
		break;
	}
	return 0;
}

LRESULT CDeskBand::OnSize(LPARAM /*lParam*/)
{
	RECT rc;
	::GetClientRect(m_hWnd, &rc);

	HDWP hdwp = BeginDeferWindowPos(2);
	DeferWindowPos(hdwp, m_hWndToolbar, NULL, 0, 0, m_tbSize.cx, m_tbSize.cy, SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_NOZORDER);
	DeferWindowPos(hdwp, m_hWndEdit, NULL, m_tbSize.cx+SPACEBETWEENEDITANDBUTTON, 0, rc.right-rc.left-m_tbSize.cx-SPACEBETWEENEDITANDBUTTON, m_tbSize.cy, SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_NOZORDER);
	EndDeferWindowPos(hdwp);
	return 0;
}

LRESULT CDeskBand::OnMove(LPARAM /*lParam*/)
{
	RECT rc;
	::GetClientRect(m_hWnd, &rc);

	HDWP hdwp = BeginDeferWindowPos(2);
	DeferWindowPos(hdwp, m_hWndToolbar, NULL, 0, 0, m_tbSize.cx, m_tbSize.cy, SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_NOZORDER);
	DeferWindowPos(hdwp, m_hWndEdit, NULL, m_tbSize.cx+SPACEBETWEENEDITANDBUTTON, 0, rc.right-rc.left-m_tbSize.cx-SPACEBETWEENEDITANDBUTTON, m_tbSize.cy, SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_NOZORDER);
	EndDeferWindowPos(hdwp);
	return 0;
}

void CDeskBand::FocusChange(BOOL bFocus)
{
	m_bFocus = bFocus;

	// inform the input object site that the focus has changed
	if (m_pSite)
	{
		m_pSite->OnFocusChangeIS((IDockingWindow*)this, bFocus);
	}
}

LRESULT CDeskBand::OnSetFocus(void)
{
	FocusChange(TRUE);
	::SetFocus(m_hWndEdit);
	return 0;
}

LRESULT CDeskBand::OnKillFocus(void)
{
	FocusChange(FALSE);

	return 0;
}

BOOL CDeskBand::RegisterAndCreateWindow(void)
{
	// If the window doesn't exist yet, create it now.
	if (!m_hWnd)
	{
		// Can't create a child window without a parent.
		if (!m_hwndParent)
		{
			return FALSE;
		}

		// If the window class has not been registered, then do so.
		WNDCLASS wc;
		if (!GetClassInfo(g_hInst, DB_CLASS_NAME, &wc))
		{
			ZeroMemory(&wc, sizeof(wc));
			wc.style          = CS_HREDRAW | CS_VREDRAW | CS_GLOBALCLASS;
			wc.lpfnWndProc    = (WNDPROC)WndProc;
			wc.cbClsExtra     = 0;
			wc.cbWndExtra     = 0;
			wc.hInstance      = g_hInst;
			wc.hIcon          = NULL;
			wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
			wc.hbrBackground  = (HBRUSH)(COLOR_BTNFACE+1);
			wc.lpszMenuName   = NULL;
			wc.lpszClassName  = DB_CLASS_NAME;

			if (!RegisterClass(&wc))
			{
				return FALSE;
			}
		}

		RECT  rc;

		GetClientRect(m_hwndParent, &rc);

		//Create the window. The WndProc will set m_hWnd.
		CreateWindowEx(0,
			DB_CLASS_NAME,
			NULL,
			WS_CHILD | WS_CLIPSIBLINGS,
			rc.left,
			rc.top,
			rc.right - rc.left,
			rc.bottom - rc.top,
			m_hwndParent,
			NULL,
			g_hInst,
			(LPVOID)this);

		GetClientRect(m_hWnd, &rc);

		// create an edit control
		m_hWndEdit = CreateWindowEx(0,
			L"EDIT",
			NULL,
			WS_VISIBLE | WS_TABSTOP | WS_CHILD | WS_CLIPSIBLINGS | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
			rc.left,
			rc.top,
			rc.right - rc.left - BUTTONSIZEX - SPACEBETWEENEDITANDBUTTON,
			rc.bottom - rc.top,
			m_hWnd,
			NULL,
			g_hInst,
			NULL);

		if (m_hWndEdit == NULL)
			return FALSE;

		// subclass the edit control to intercept the WM_SETFOCUS messages
		m_oldEditWndProc = (WNDPROC)SetWindowLongPtr(m_hWndEdit, GWL_WNDPROC, (LONG)EditProc);
		SetWindowLongPtr(m_hWndEdit, GWL_USERDATA, (LONG)this);

		// set the font for the edit control
		SendMessage(m_hWndEdit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), 0);

		// create a toolbar which will hold our button
		m_hWndToolbar = CreateWindowEx(0,
			TOOLBARCLASSNAME, 
			NULL, 
			WS_CHILD|TBSTYLE_LIST|TBSTYLE_FLAT|TBSTYLE_TRANSPARENT|CCS_NORESIZE|CCS_NODIVIDER|CCS_NOPARENTALIGN, 
			rc.right - BUTTONSIZEX,
			rc.top,
			BUTTONSIZEX,
			rc.bottom - rc.top,
			m_hWnd, 
			NULL, 
			g_hInst, 
			NULL);

		// Send the TB_BUTTONSTRUCTSIZE message, which is required for 
		// backward compatibility. 
		SendMessage(m_hWndToolbar, TB_BUTTONSTRUCTSIZE, (WPARAM) sizeof(TBBUTTON), 0); 
		
		ShowWindow(m_hWndToolbar, SW_SHOW); 
	}
	return (NULL != m_hWnd);
}

LRESULT CALLBACK CDeskBand::KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
	DWORD threadID = GetCurrentThreadId();
	std::map<DWORD, CDeskBand*>::iterator it = m_desklist.find(threadID);
	if (it != m_desklist.end())
	{
		if (wParam == 'S' )//its about S key
		{
			if (GetKeyState(VK_CONTROL)&0x8000)// and Ctrl is currently pressed
			{
				if ((lParam & 0xc0000000) == 0)//key went from 'up' to 'down' state
				{	
					if (it->second->m_pSite)
					{
						it->second->OnSetFocus();
						return 1;//we processed it
					}
				}
			}
		}
		return CallNextHookEx(it->second->m_hook, code, wParam, lParam);
	}
	return 0;
}

BOOL CDeskBand::BuildToolbarButtons()
{
	if (m_hWndToolbar == NULL)
		return FALSE;

	// first remove all existing buttons to start from scratch
	LRESULT buttoncount = ::SendMessage(m_hWndToolbar, TB_BUTTONCOUNT, 0, 0);
	for (int i=0; i<buttoncount; ++i)
	{
		::SendMessage(m_hWndToolbar, TB_DELETEBUTTON, 0, 0);
	}
	// destroy the image list
	ImageList_Destroy(m_hToolbarImgList);

	// find custom commands
	CStdRegistryKey regkeys = CStdRegistryKey(_T("Software\\StefansTools\\"));
	stdregistrykeylist subkeys;
	regkeys.getSubKeys(subkeys);

	TBBUTTON * tb = new TBBUTTON[subkeys.size()+NUMINTERNALCOMMANDS];

	// create an image list containing the icons for the toolbar
	m_hToolbarImgList = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, subkeys.size()+NUMINTERNALCOMMANDS, 1);
	if (m_hToolbarImgList == NULL)
	{
		delete [] tb;
		return false;
	}

	// now add the default command buttons
	HICON hIcon = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_OPTIONS));
	tb[0].iBitmap = ImageList_AddIcon(m_hToolbarImgList, hIcon);;
	tb[0].idCommand = 1;
	tb[0].fsState = TBSTATE_ENABLED;
	tb[0].fsStyle = BTNS_BUTTON|BTNS_SHOWTEXT;
	tb[0].iString = (INT_PTR)_T("Options");
	DestroyIcon(hIcon);

	hIcon = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_CMD));
	tb[1].iBitmap = ImageList_AddIcon(m_hToolbarImgList, hIcon);;
	tb[1].idCommand = 2;
	tb[1].fsState = TBSTATE_ENABLED;
	tb[1].fsStyle = BTNS_BUTTON|BTNS_SHOWTEXT;
	tb[1].iString = 0;
	DestroyIcon(hIcon);

	hIcon = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_COPYNAME));
	tb[2].iBitmap = ImageList_AddIcon(m_hToolbarImgList, hIcon);;
	tb[2].idCommand = 3;
	tb[2].fsState = TBSTATE_ENABLED;
	tb[2].fsStyle = BTNS_BUTTON|BTNS_SHOWTEXT;
	tb[2].iString = 0;
	DestroyIcon(hIcon);

	hIcon = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_COPYPATH));
	tb[3].iBitmap = ImageList_AddIcon(m_hToolbarImgList, hIcon);;
	tb[3].idCommand = 4;
	tb[3].fsState = TBSTATE_ENABLED;
	tb[3].fsStyle = BTNS_BUTTON|BTNS_SHOWTEXT;
	tb[3].iString = 0;
	DestroyIcon(hIcon);

	int customindex = NUMINTERNALCOMMANDS;
	for (stdregistrykeylist::iterator it = subkeys.begin(); it != subkeys.end(); ++it)
	{
		customindex++;
		stdstring key = _T("Software\\StefansTools\\");
		key += it->c_str();
		stdstring sIcon = key + _T("\\icon");
		CRegStdString regicon = CRegStdString(sIcon);
		hIcon = LoadIcon(g_hInst, sIcon.c_str());
		tb[customindex].iBitmap = ImageList_AddIcon(m_hToolbarImgList, hIcon);;
		tb[customindex].idCommand = 4;
		tb[customindex].fsState = TBSTATE_ENABLED;
		tb[customindex].fsStyle = BTNS_BUTTON|BTNS_SHOWTEXT;
		tb[customindex].iString = (INT_PTR)it->substr(1).c_str();
		DestroyIcon(hIcon);
	}

	SendMessage(m_hWndToolbar, TB_SETIMAGELIST, 0, (LPARAM)m_hToolbarImgList);
	SendMessage(m_hWndToolbar, TB_ADDBUTTONS, subkeys.size()+NUMINTERNALCOMMANDS, (LPARAM)tb);
	SendMessage(m_hWndToolbar, TB_AUTOSIZE, 0, 0);
	SendMessage(m_hWndToolbar, TB_GETMAXSIZE, 0,(LPARAM)&m_tbSize);
	delete [] tb;
	return TRUE;
}