// Fetching remote pictures (plan 2.3). Everything here runs on worker threads, never before the first frame.
#pragma once
#include "common.h"

// Downloads https:// (and http:// only when allowHttp) into out. Returns false on any error, timeout or size limit.
bool HttpGet(const std::wstring& url, std::vector<uint8_t>& out, size_t maxBytes, bool allowHttp);

// Where a URL is kept on disk: %LOCALAPPDATA%\FastMD\cache\<hash>.bin (FASTMD_DATA overrides the root, as elsewhere).
std::wstring CacheFileFor(const std::wstring& url);

// Drops the least recently used files once the cache passes the budget.
void TrimHttpCache(uint64_t budgetBytes = 200ull << 20);
