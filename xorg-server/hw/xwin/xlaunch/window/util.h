/*
 * Copyright (c) 2005 Alexander Gottwald
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE ABOVE LISTED COPYRIGHT HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name(s) of the above copyright
 * holders shall not be used in advertising or otherwise to promote the sale,
 * use or other dealings in this Software without prior written authorization.
 */
#ifndef __UTIL_H__
#define __UTIL_H__

#include <windows.h>
#include <stdexcept>
#include <string>  

class win32_error : public std::runtime_error
{
    public:
        static std::string message(DWORD code);
        DWORD errorcode;
        win32_error(const std::string &msg,DWORD code = GetLastError()) : std::runtime_error(msg + ":" + message(code)), errorcode(code) {};
};

#define MESSAGE_NAMES_LEN 1024
#define NOTIFY_NAMES_LEN 0
class MessageDebug
{
    protected:
        static const char * message_names[MESSAGE_NAMES_LEN];
        static const char * notify_names[NOTIFY_NAMES_LEN];
    public:
        static void debug(HWND handle, UINT uMsg, WPARAM wParam, LPARAM lParam, const char *prefix);
};


#endif
