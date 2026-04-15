# OSINT-AI Project - Remaining Issues & Fixes

**Date**: April 14, 2026  
**Status**: Build failing with 3 identified issues  

---

## Issues to Fix

### 1. ✅ FIXED: AIBrain.cpp Missing Header
**Status**: ✓ DONE  
Added: `#include <set>`

### 2. ✅ FIXED: WebScraper.cpp Raw String Delimiter  
**Status**: ✓ DONE  
Fixed Line 213 regex pattern closing delimiter

### 3. ⚠️ TODO: SearchEngine.cpp Raw String Parsing
**Lines**: 126, 131  
**Issue**: Raw string literals `R"..."` not parsing correctly  
**Error Pattern**:
```
expected primary-expression before '.*' token
```

**Current Code (Lines 123-131)**:
```cpp
std::regex link_re(
    R"(<a[^>]+class="result__a"[^>]+href="([^"]+)"[^>]*>(.*?)</a>)",
    std::regex::icase | std::regex::ECMAScript
);
std::regex snip_re(
    R"(<a[^>]+class="result__snippet"[^>]*>(.*?)</a>)",
    std::regex::icase | std::regex::ECMAScript
);
```

**Solution**: Replace with regular strings with proper escaping:
```cpp
std::regex link_re(
    "<a[^>]+class=\"result__a\"[^>]+href=\"([^\"]+)\"[^>]*>(.*?)</a>",
    std::regex::icase | std::regex::ECMAScript
);
std::regex snip_re(
    "<a[^>]+class=\"result__snippet\"[^>]*>(.*?)</a>",
    std::regex::icase | std::regex::ECMAScript
);
```

### 4. ⚠️ TODO: CLI.cpp String Concatenation Error
**Lines**: 83, 169, 181, 195  
**Issue**: Trying to add `const char*` constants with `+` operator

**Current Code (Line 83)**:
```cpp
std::cout << c(col::BOLD + col::CYAN, "osint> ") << std::flush;
```

**Solution**: Use `std::string` concatenation or use separate calls:
```cpp
std::string prompt = col::BOLD;
prompt += col::CYAN;
std::cout << c(prompt, "osint> ") << std::flush;

// OR use fmt::format
std::cout << c(fmt::format("{}{}", col::BOLD, col::CYAN), "osint> ") << std::flush;
```

---

## Quick Fix Application

### For SearchEngine.cpp (Lines 123-136):
Replace the entire regex initialization block...

### For CLI.cpp (Lines 83, 169, 181, 195):
Replace color concatenation with proper string operations...

---

## Build Command to Test

```bash
cd /home/hiori/Desktop/WorkStation/Agent
bash build.sh release
```

If all fixes are applied, should see:
```
[build] ✓ Build successful!
[build]   Executable: ./build/osint_ai
```

---

**Next Step**: Apply these fixes and rebuild
