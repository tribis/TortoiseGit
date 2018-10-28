// TortoiseGit - a Windows shell extension for easy version control

// Copyright (C) 2017-2018 - TortoiseGit
// Copyright (C) 2003-2016 - TortoiseSVN

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

#include "stdafx.h"
#include "GitWCRev.h"
#include "status.h"
#include "registry.h"
#include "StringUtils.h"
#include "UnicodeUtils.h"
#include <fstream>
#include <ShlObj.h>
#include "git2/sys/repository.h"

void LoadIgnorePatterns(const char* wc, GitWCRev_t* GitStat)
{
	std::string path = wc;
	std::string ignorepath = path + "/.GitWCRevignore";

	std::ifstream infile;
	infile.open(ignorepath);
	if (!infile.good())
		return;

	GitStat->ignorepatterns.emplace("*");

	std::string line;
	while (std::getline(infile, line))
	{
		if (line.empty())
			continue;

		line.insert(line.begin(), '!');
		GitStat->ignorepatterns.emplace(line);
	}
}

static std::wstring GetHomePath()
{
	wchar_t* tmp;
	if ((tmp = _wgetenv(L"HOME")) != nullptr && *tmp)
		return tmp;

	if ((tmp = _wgetenv(L"HOMEDRIVE")) != nullptr)
	{
		std::wstring home(tmp);
		if ((tmp = _wgetenv(L"HOMEPATH")) != nullptr)
		{
			home.append(tmp);
			if (PathIsDirectory(home.c_str()))
				return home;
		}
	}

	if ((tmp = _wgetenv(L"USERPROFILE")) != nullptr && *tmp)
		return tmp;

	return {};
}

static int is_cygwin_msys2_hack_active()
{
	HKEY hKey;
	DWORD dwType = REG_DWORD;
	DWORD dwValue = 0;
	DWORD dwSize = sizeof(dwValue);
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\TortoiseGit", 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS)
	{
		RegQueryValueExW(hKey, L"CygwinHack", nullptr, &dwType, (LPBYTE)&dwValue, &dwSize);
		if (dwValue != 1)
			RegQueryValueExW(hKey, L"Msys2Hack", nullptr, &dwType, (LPBYTE)&dwValue, &dwSize);
		RegCloseKey(hKey);
	}
	return dwValue == 1;
}

static std::wstring GetProgramDataConfig()
{
	wchar_t wbuffer[MAX_PATH];

	// do not use shared windows-wide system config when cygwin hack is active
	if (is_cygwin_msys2_hack_active())
		return {};

	if (SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, wbuffer) != S_OK || wcslen(wbuffer) >= MAX_PATH - wcslen(L"\\Git\\config"))
		return{};

	wcscat(wbuffer, L"\\Git\\config");

	return wbuffer;
}

static std::wstring GetSystemGitConfig()
{
	HKEY hKey;
	DWORD dwType = REG_SZ;
	TCHAR path[MAX_PATH] = { 0 };
	DWORD dwSize = _countof(path) - 1;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\TortoiseGit", 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS)
	{
		RegQueryValueExW(hKey, L"SystemConfig", nullptr, &dwType, (LPBYTE)&path, &dwSize);
		RegCloseKey(hKey);
	}
	return path;
}

int GetStatus(const TCHAR* path, GitWCRev_t& GitStat)
{
	std::string pathA = CUnicodeUtils::StdGetUTF8(path);
	CAutoBuf dotgitdir;
	if (git_repository_discover(dotgitdir, pathA.c_str(), 0, nullptr) < 0)
		return ERR_NOWC;

	CAutoRepository repo;
	if (git_repository_open(repo.GetPointer(), dotgitdir->ptr))
		return ERR_NOWC;

	CAutoConfig config(true);
	std::string gitdir(dotgitdir->ptr, dotgitdir->size);
	git_config_add_file_ondisk(config, (gitdir + "config").c_str(), GIT_CONFIG_LEVEL_LOCAL, repo, FALSE);
	std::string home(CUnicodeUtils::StdGetUTF8(GetHomePath()));
	git_config_add_file_ondisk(config, (home + "\\.gitconfig").c_str(), GIT_CONFIG_LEVEL_GLOBAL, repo, FALSE);
	git_config_add_file_ondisk(config, (home + "\\.config\\git\\config").c_str(), GIT_CONFIG_LEVEL_XDG, repo, FALSE);
	std::wstring systemConfig = GetSystemGitConfig();
	if (!systemConfig.empty())
		git_config_add_file_ondisk(config, CUnicodeUtils::StdGetUTF8(systemConfig).c_str(), GIT_CONFIG_LEVEL_SYSTEM, repo, FALSE);
	std::wstring programDataConfig = GetProgramDataConfig();
	if (!programDataConfig.empty())
		git_config_add_file_ondisk(config, CUnicodeUtils::StdGetUTF8(programDataConfig).c_str(), GIT_CONFIG_LEVEL_PROGRAMDATA, repo, FALSE);
	git_repository_set_config(repo, config);

	if (git_repository_head_unborn(repo))
	{
		memset(GitStat.HeadHash, 0, sizeof(GitStat.HeadHash));
		strncpy_s(GitStat.HeadHashReadable, GIT_OID_HEX_ZERO, strlen(GIT_OID_HEX_ZERO));
		GitStat.bIsUnborn = TRUE;

		CAutoReference head;
		if (git_repository_head(head.GetPointer(), repo) != GIT_EUNBORNBRANCH)
			return ERR_GIT_ERR;
		GitStat.CurrentBranch = git_reference_shorthand(head);

		return 0;
	}

	CAutoReference head;
	if (git_repository_head(head.GetPointer(), repo) < 0)
		return ERR_GIT_ERR;
	GitStat.CurrentBranch = git_reference_shorthand(head);

	CAutoObject object;
	if (git_reference_peel(object.GetPointer(), head, GIT_OBJ_COMMIT) < 0)
		return ERR_GIT_ERR;

	const git_oid* oid = git_object_id(object);
	git_oid_cpy((git_oid*)GitStat.HeadHash, oid);
	git_oid_tostr(GitStat.HeadHashReadable, sizeof(GitStat.HeadHashReadable), oid);

	CAutoCommit commit;
	if (git_commit_lookup(commit.GetPointer(), repo, oid) < 0)
		return ERR_GIT_ERR;

	const git_signature* sig = git_commit_author(commit);
	GitStat.HeadAuthor = sig->name;
	GitStat.HeadEmail = sig->email;
	GitStat.HeadTime = sig->when.time;

#pragma warning(push)
#pragma warning(disable: 4510 4512 4610)
	struct TagPayload { git_repository* repo; GitWCRev_t& GitStat; } tagpayload = { repo, GitStat };
#pragma warning(pop)

	if (git_tag_foreach(repo, [](const char*, git_oid* tagoid, void* payload)
	{
		auto pl = reinterpret_cast<struct TagPayload*>(payload);
		if (git_oid_cmp(tagoid, (git_oid*)pl->GitStat.HeadHash) == 0)
		{
			pl->GitStat.bIsTagged = TRUE;
			return 0;
		}

		CAutoTag tag;
		if (git_tag_lookup(tag.GetPointer(), pl->repo, tagoid))
			return 0; // not an annotated tag
		CAutoObject tagObject;
		if (git_tag_peel(tagObject.GetPointer(), tag))
			return -1;
		if (git_oid_cmp(git_object_id(tagObject), (git_oid*)pl->GitStat.HeadHash) == 0)
			pl->GitStat.bIsTagged = TRUE;

		return 0;
	}, &tagpayload))
		return ERR_GIT_ERR;

	git_status_options git_status_options = GIT_STATUS_OPTIONS_INIT;
	git_status_options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
	if (GitStat.bNoSubmodules)
		git_status_options.flags |= GIT_STATUS_OPT_EXCLUDE_SUBMODULES;

	std::string workdir(git_repository_workdir(repo));
	LoadIgnorePatterns(workdir.c_str(), &GitStat);

	std::vector<char*> pathspec;
	if (!GitStat.ignorepatterns.empty())
	{
		for (auto& i : GitStat.ignorepatterns)
			pathspec.emplace_back(const_cast<char*>(i.c_str()));
		git_status_options.pathspec.count = GitStat.ignorepatterns.size();
		git_status_options.pathspec.strings = &pathspec.at(0);
	}

	CAutoStatusList status;
	if (git_status_list_new(status.GetPointer(), repo, &git_status_options) < 0)
		return ERR_GIT_ERR;

	for (size_t i = 0, maxi = git_status_list_entrycount(status); i < maxi; ++i)
	{
		const git_status_entry* s = git_status_byindex(status, i);
		if (s->index_to_workdir && s->index_to_workdir->new_file.mode == GIT_FILEMODE_COMMIT)
		{
			GitStat.bHasSubmodule = TRUE;
			unsigned int smstatus = 0;
			if (!git_submodule_status(&smstatus, repo, s->index_to_workdir->new_file.path, GIT_SUBMODULE_IGNORE_UNSPECIFIED))
			{
				if (smstatus & GIT_SUBMODULE_STATUS_WD_MODIFIED) // HEAD of submodule not matching
					GitStat.bHasSubmoduleNewCommits = TRUE;
				else if ((smstatus & GIT_SUBMODULE_STATUS_WD_INDEX_MODIFIED) || (smstatus & GIT_SUBMODULE_STATUS_WD_WD_MODIFIED))
					GitStat.bHasSubmoduleMods = TRUE;
				else if (smstatus & GIT_SUBMODULE_STATUS_WD_UNTRACKED)
					GitStat.bHasSubmoduleUnversioned = TRUE;
			}
			continue;
		}
		if (s->status == GIT_STATUS_CURRENT)
			continue;
		if (s->status == GIT_STATUS_WT_NEW)
			GitStat.HasUnversioned = TRUE;
		else
			GitStat.HasMods = TRUE;
	}

	// count the first-parent revisions from HEAD to the first commit
	CAutoRevwalk walker;
	if (git_revwalk_new(walker.GetPointer(), repo) < 0)
		return ERR_GIT_ERR;
	git_revwalk_simplify_first_parent(walker);
	if (git_revwalk_push_head(walker) < 0)
		return ERR_GIT_ERR;
	git_oid oidlog;
	while (!git_revwalk_next(&oidlog, walker))
		++GitStat.NumCommits;

	std::transform(pathA.begin(), pathA.end(), pathA.begin(), [](char c) { return (c == '\\') ? '/' : c; });
	pathA.erase(pathA.begin(), pathA.begin() + min(workdir.length(), pathA.length())); // workdir always ends with a slash, however, wcA is not guaranteed to
	if (pathA.empty()) // working tree root is always versioned
	{
		GitStat.bIsGitItem = TRUE;
		return 0;
	}
	unsigned int status_flags = 0;
	int ret = git_status_file(&status_flags, repo, pathA.c_str());
	GitStat.bIsGitItem = (ret == GIT_EAMBIGUOUS || (ret == 0 && !(status_flags & GIT_STATUS_IGNORED)));
	return 0;
}
