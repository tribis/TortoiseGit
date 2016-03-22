// TortoiseGit - a Windows shell extension for easy version control

// Copyright (C) 2009-2013, 2015-2016 - TortoiseGit

// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//
#include "stdafx.h"
#include "SVNRebaseCommand.h"

#include "SysProgressDlg.h"
#include "ProgressDlg.h"
#include "MessageBox.h"
#include "Git.h"
#include "RebaseDlg.h"
#include "AppUtils.h"

bool SVNRebaseCommand::Execute()
{
	bool isStash = false;

	if(!g_Git.CheckCleanWorkTree())
	{
		if (CMessageBox::Show(NULL, g_Git.m_CurrentDir + _T("\r\n") + CString(MAKEINTRESOURCE(IDS_ERROR_NOCLEAN_STASH)), _T("TortoiseGit"), 1, IDI_QUESTION, CString(MAKEINTRESOURCE(IDS_STASHBUTTON)), CString(MAKEINTRESOURCE(IDS_ABORTBUTTON))) == 1)
		{
			CSysProgressDlg sysProgressDlg;
			sysProgressDlg.SetTitle(CString(MAKEINTRESOURCE(IDS_APPNAME)));
			sysProgressDlg.SetLine(1, CString(MAKEINTRESOURCE(IDS_PROC_STASHRUNNING)));
			sysProgressDlg.SetLine(2, CString(MAKEINTRESOURCE(IDS_PROGRESSWAIT)));
			sysProgressDlg.SetShowProgressBar(false);
			sysProgressDlg.SetCancelMsg(IDS_PROGRS_INFOFAILED);
			sysProgressDlg.ShowModeless((HWND)NULL, true);

			CString cmd,out;
			cmd=_T("git.exe stash");
			if (g_Git.Run(cmd, &out, CP_UTF8))
			{
				sysProgressDlg.Stop();
				MessageBox(hwndExplorer, out, _T("TortoiseGit"), MB_OK | MB_ICONERROR);
				return false;
			}
			sysProgressDlg.Stop();
			isStash = true;
		}
		else
		{
			return false;
		}
	}

	CRebaseDlg dlg;

//	dlg.m_PreCmd=_T("git.exe svn fetch");

	CString cmd, out, err;
	cmd = _T("git.exe config svn-remote.svn.fetch");

	if (!g_Git.Run(cmd, &out, &err, CP_UTF8))
	{
		int start = out.Find(_T(':'));
		if( start >=0 )
			out=out.Mid(start);

		if(out.Left(5) == _T(":refs"))
			out=out.Mid(6);

		start = 0;
		out=out.Tokenize(_T("\n"),start);
	}
	else
	{
		CMessageBox::Show(NULL, out + L"\n" + err, _T("TortoiseGit"), MB_OK|MB_ICONERROR);
		return false;
	}

	dlg.m_Upstream=out;

	CGitHash UpStreamOldHash,HeadHash,UpStreamNewHash;
	if (g_Git.GetHash(UpStreamOldHash, out))
	{
		MessageBox(hwndExplorer, g_Git.GetGitLastErr(_T("Could not get hash of SVN branch.")), _T("TortoiseGit"), MB_ICONERROR);
		return false;
	}
	if (g_Git.GetHash(HeadHash, _T("HEAD")))
	{
		MessageBox(hwndExplorer, g_Git.GetGitLastErr(_T("Could not get HEAD hash.")), _T("TortoiseGit"), MB_ICONERROR);
		return false;
	}
	CProgressDlg progress;
	progress.m_GitCmd=_T("git.exe svn fetch");
	progress.m_AutoClose = AUTOCLOSE_IF_NO_ERRORS;

	if(progress.DoModal()!=IDOK)
		return false;

	if(progress.m_GitStatus)
		return false;

	if (g_Git.GetHash(UpStreamNewHash, out))
	{
		MessageBox(hwndExplorer, g_Git.GetGitLastErr(_T("Could not get upstream hash after fetching.")), _T("TortoiseGit"), MB_ICONERROR);
		return false;
	}

	//everything updated
	if(UpStreamNewHash==HeadHash)
	{
		MessageBox(hwndExplorer, g_Git.m_CurrentDir + _T("\r\n") + CString(MAKEINTRESOURCE(IDS_PROC_EVERYTHINGUPDATED)), _T("TortoiseGit"), MB_OK | MB_ICONQUESTION);
		if(isStash)
			askIfUserWantsToStashPop();

		return true;
	}

	//fast forward;
	if(g_Git.IsFastForward(CString(_T("HEAD")),out))
	{
		CProgressDlg progressReset;
		cmd.Format(_T("git.exe reset --hard %s --"), (LPCTSTR)out);
		progressReset.m_GitCmd = cmd;
		progressReset.m_AutoClose = AUTOCLOSE_IF_NO_ERRORS;

		if (progressReset.DoModal() != IDOK)
			return false;
		else
		{
			MessageBox(hwndExplorer, g_Git.m_CurrentDir + _T("\r\n") + CString(MAKEINTRESOURCE(IDS_PROC_FASTFORWARD)) + _T(":\n") + progressReset.m_LogText, _T("TortoiseGit"), MB_OK | MB_ICONQUESTION);
			if(isStash)
				askIfUserWantsToStashPop();

			return true;
		}
	}

	dlg.m_PostButtonTexts.Add(CString(MAKEINTRESOURCE(IDS_MENULOG)));
	//need rebase
	INT_PTR response = dlg.DoModal();
	if (response == IDOK || response == IDC_REBASE_POST_BUTTON)
	{
		if(isStash)
			askIfUserWantsToStashPop();
		if (response == IDC_REBASE_POST_BUTTON)
		{
			cmd = _T("/command:log");
			cmd += _T(" /path:\"") + g_Git.m_CurrentDir + _T("\"");
			CAppUtils::RunTortoiseGitProc(cmd);
		}
		return true;
	}
	return false;
}

void SVNRebaseCommand::askIfUserWantsToStashPop()
{
	if (CMessageBox::Show(NULL, g_Git.m_CurrentDir + _T("\r\n") + CString(MAKEINTRESOURCE(IDS_DCOMMIT_STASH_POP)), _T("TortoiseGit"), MB_YESNO|MB_ICONINFORMATION) == IDYES)
	{
		CAppUtils::StashPop();
	}
}
