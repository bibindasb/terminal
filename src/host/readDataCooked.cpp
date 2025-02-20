// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "readDataCooked.hpp"

#include "alias.h"
#include "history.h"
#include "resource.h"
#include "stream.h"
#include "_stream.h"
#include "../interactivity/inc/ServiceLocator.hpp"

using Microsoft::Console::Interactivity::ServiceLocator;

// As per https://graphics.stanford.edu/~seander/bithacks.html#IntegerLog10Obvious
constexpr int integerLog10(uint32_t v)
{
    return (v >= 1000000000) ? 9 :
           (v >= 100000000)  ? 8 :
           (v >= 10000000)   ? 7 :
           (v >= 1000000)    ? 6 :
           (v >= 100000)     ? 5 :
           (v >= 10000)      ? 4 :
           (v >= 1000)       ? 3 :
           (v >= 100)        ? 2 :
           (v >= 10)         ? 1 :
                               0;
}

// Routine Description:
// - Constructs cooked read data class to hold context across key presses while a user is modifying their 'input line'.
// Arguments:
// - pInputBuffer - Buffer that data will be read from.
// - pInputReadHandleData - Context stored across calls from the same input handle to return partial data appropriately.
// - screenInfo - Output buffer that will be used for 'echoing' the line back to the user so they can see/manipulate it
// - UserBufferSize - The byte count of the buffer presented by the client
// - UserBuffer - The buffer that was presented by the client for filling with input data on read conclusion/return from server/host.
// - CtrlWakeupMask - Special client parameter to interrupt editing, end the wait, and return control to the client application
// - initialData - any text data that should be prepopulated into the buffer
// - pClientProcess - Attached process handle object
COOKED_READ_DATA::COOKED_READ_DATA(_In_ InputBuffer* const pInputBuffer,
                                   _In_ INPUT_READ_HANDLE_DATA* const pInputReadHandleData,
                                   SCREEN_INFORMATION& screenInfo,
                                   _In_ size_t UserBufferSize,
                                   _In_ char* UserBuffer,
                                   _In_ ULONG CtrlWakeupMask,
                                   _In_ const std::wstring_view exeName,
                                   _In_ const std::wstring_view initialData,
                                   _In_ ConsoleProcessHandle* const pClientProcess) :
    ReadData(pInputBuffer, pInputReadHandleData),
    _screenInfo{ screenInfo },
    _userBuffer{ UserBuffer, UserBufferSize },
    _exeName{ exeName },
    _processHandle{ pClientProcess },
    _history{ CommandHistory::s_Find(pClientProcess) },
    _ctrlWakeupMask{ CtrlWakeupMask },
    _insertMode{ ServiceLocator::LocateGlobals().getConsoleInformation().GetInsertMode() }
{
#ifndef UNIT_TESTING
    // The screen buffer instance is basically a reference counted HANDLE given out to the user.
    // We need to ensure that it stays alive for the duration of the read.
    // Coincidentally this serves another important purpose: It checks whether we're allowed to read from
    // the given buffer in the first place. If it's missing the FILE_SHARE_READ flag, we can't read from it.
    THROW_IF_FAILED(_screenInfo.AllocateIoHandle(ConsoleHandleData::HandleType::Output, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, _tempHandle));
#endif

    if (!initialData.empty())
    {
        _buffer.assign(initialData);
        _bufferCursor = _buffer.size();
        _bufferDirty = !_buffer.empty();

        // The console API around `nInitialChars` in `CONSOLE_READCONSOLE_CONTROL` is pretty weird.
        // The way it works is that cmd.exe does a ReadConsole() with a `dwCtrlWakeupMask` that includes \t,
        // so when you press tab it can autocomplete the prompt based on the available file names.
        // The weird part is that it's not us who then prints the autocompletion. It's cmd.exe which calls WriteConsoleW().
        // It then initiates another ReadConsole() where the `nInitialChars` is the amount of chars it wrote via WriteConsoleW().
        //
        // In other words, `nInitialChars` is a "trust me bro, I just wrote that in the buffer" API.
        // This unfortunately means that the API is inherently broken: ReadConsole() visualizes control
        // characters like Ctrl+X as "^X" and WriteConsoleW() doesn't and so the column counts don't match.
        // Solving these issues is technically possible, but it's also quite difficult to do so correctly.
        //
        // But unfortunately (or fortunately) the initial (from the 1990s up to 2023) looked something like that:
        //   cursor = cursor.GetPosition();
        //   cursor.x -= initialData.size();
        //   while (cursor.x < 0)
        //   {
        //       cursor.x += textBuffer.Width();
        //       cursor.y -= 1;
        //   }
        //
        // In other words, it assumed that the number of code units in the initial data corresponds 1:1 to
        // the column count. This meant that the API never supported tabs for instance (nor wide glyphs).
        // The new implementation still doesn't support tabs, but it does fix support for wide glyphs.
        // That seemed like a good trade-off.

        // NOTE: You can't just "measure" the length of the string in columns either, because previously written
        // wide glyphs might have resulted in padding whitespace in the text buffer (see ROW::WasDoubleBytePadded).
        // The alternative to the loop below is counting the number of padding glyphs while iterating backwards. Either approach is fine.
        til::CoordType distance = 0;
        for (size_t i = 0; i < initialData.size(); i = TextBuffer::GraphemeNext(initialData, i))
        {
            --distance;
        }

        const auto& textBuffer = _screenInfo.GetTextBuffer();
        const auto& cursor = textBuffer.GetCursor();
        const auto end = cursor.GetPosition();
        const auto beg = textBuffer.NavigateCursor(end, distance);
        _distanceCursor = (end.y - beg.y) * textBuffer.GetSize().Width() + end.x - beg.x;
        _distanceEnd = _distanceCursor;
    }
}

// Routine Description:
// - This routine is called to complete a cooked read that blocked in ReadInputBuffer.
// - The context of the read was saved in the CookedReadData structure.
// - This routine is called when events have been written to the input buffer.
// - It is called in the context of the writing thread.
// - It may be called more than once.
// Arguments:
// - TerminationReason - if this routine is called because a ctrl-c or ctrl-break was seen, this argument
//                      contains CtrlC or CtrlBreak. If the owning thread is exiting, it will have ThreadDying. Otherwise 0.
// - fIsUnicode - Whether to convert the final data to A (using Console Input CP) at the end or treat everything as Unicode (UCS-2)
// - pReplyStatus - The status code to return to the client application that originally called the API (before it was queued to wait)
// - pNumBytes - The number of bytes of data that the server/driver will need to transmit back to the client process
// - pControlKeyState - For certain types of reads, this specifies which modifier keys were held.
// - pOutputData - not used
// Return Value:
// - true if the wait is done and result buffer/status code can be sent back to the client.
// - false if we need to continue to wait until more data is available.
bool COOKED_READ_DATA::Notify(const WaitTerminationReason TerminationReason,
                              const bool fIsUnicode,
                              _Out_ NTSTATUS* const pReplyStatus,
                              _Out_ size_t* const pNumBytes,
                              _Out_ DWORD* const pControlKeyState,
                              _Out_ void* const /*pOutputData*/) noexcept
try
{
    auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    *pNumBytes = 0;
    *pControlKeyState = 0;
    *pReplyStatus = STATUS_SUCCESS;

    // if ctrl-c or ctrl-break was seen, terminate read.
    if (WI_IsAnyFlagSet(TerminationReason, (WaitTerminationReason::CtrlC | WaitTerminationReason::CtrlBreak)))
    {
        *pReplyStatus = STATUS_ALERTED;
        gci.SetCookedReadData(nullptr);
        return true;
    }

    // See if we were called because the thread that owns this wait block is exiting.
    if (WI_IsFlagSet(TerminationReason, WaitTerminationReason::ThreadDying))
    {
        *pReplyStatus = STATUS_THREAD_IS_TERMINATING;
        gci.SetCookedReadData(nullptr);
        return true;
    }

    // We must see if we were woken up because the handle is being closed. If
    // so, we decrement the read count. If it goes to zero, we wake up the
    // close thread. Otherwise, we wake up any other thread waiting for data.
    if (WI_IsFlagSet(TerminationReason, WaitTerminationReason::HandleClosing))
    {
        *pReplyStatus = STATUS_ALERTED;
        gci.SetCookedReadData(nullptr);
        return true;
    }

    if (Read(fIsUnicode, *pNumBytes, *pControlKeyState))
    {
        gci.SetCookedReadData(nullptr);
        return true;
    }

    return false;
}
NT_CATCH_RETURN()

void COOKED_READ_DATA::MigrateUserBuffersOnTransitionToBackgroundWait(const void* oldBuffer, void* newBuffer) noexcept
{
    // See the comment in WaitBlock.cpp for more information.
    if (_userBuffer.data() == oldBuffer)
    {
        _userBuffer = { static_cast<char*>(newBuffer), _userBuffer.size() };
    }
}

// Routine Description:
// - Method that actually retrieves a character/input record from the buffer (key press form)
//   and determines the next action based on the various possible cooked read modes.
// - Mode options include the F-keys popup menus, keyboard manipulation of the edit line, etc.
// - This method also does the actual copying of the final manipulated data into the return buffer.
// Arguments:
// - isUnicode - Treat as UCS-2 unicode or use Input CP to convert when done.
// - numBytes - On in, the number of bytes available in the client
// buffer. On out, the number of bytes consumed in the client buffer.
// - controlKeyState - For some types of reads, this is the modifier key state with the last button press.
bool COOKED_READ_DATA::Read(const bool isUnicode, size_t& numBytes, ULONG& controlKeyState)
{
    controlKeyState = 0;

    const auto done = _readCharInputLoop();

    // NOTE: Don't call _flushBuffer in a wil::scope_exit/defer.
    // It may throw and throwing during an ongoing exception is a bad idea.
    _flushBuffer();

    if (done)
    {
        _handlePostCharInputLoop(isUnicode, numBytes, controlKeyState);
    }

    return done;
}

// Printing wide glyphs at the end of a row results in a forced line wrap and a padding whitespace to be inserted.
// When the text buffer resizes these padding spaces may vanish and the _distanceCursor and _distanceEnd measurements become inaccurate.
// To fix this, this function is called before a resize and will clear the input line. Afterwards, RedrawAfterResize() will restore it.
void COOKED_READ_DATA::EraseBeforeResize()
{
    _popupsDone();

    if (_distanceEnd)
    {
        _unwindCursorPosition(_distanceCursor);
        _erase(_distanceEnd);
        _unwindCursorPosition(_distanceEnd);
        _distanceCursor = 0;
        _distanceEnd = 0;
    }
}

// The counter-part to EraseBeforeResize().
void COOKED_READ_DATA::RedrawAfterResize()
{
    _markAsDirty();
    _flushBuffer();
}

void COOKED_READ_DATA::SetInsertMode(bool insertMode) noexcept
{
    _insertMode = insertMode;
}

bool COOKED_READ_DATA::IsEmpty() const noexcept
{
    return _buffer.empty() && _popups.empty();
}

bool COOKED_READ_DATA::PresentingPopup() const noexcept
{
    return !_popups.empty();
}

til::point_span COOKED_READ_DATA::GetBoundaries() const noexcept
{
    const auto& textBuffer = _screenInfo.GetTextBuffer();
    const auto& cursor = textBuffer.GetCursor();
    const auto beg = _offsetPosition(cursor.GetPosition(), -_distanceCursor);
    const auto end = _offsetPosition(beg, _distanceEnd);
    return { beg, end };
}

// _wordPrev and _wordNext implement the classic Windows word-wise cursor movement algorithm, as traditionally used by
// conhost, notepad, Visual Studio and other "old" applications. If you look closely you can see how they're the exact
// same "skip 1 char, skip x, skip not-x", but since the "x" between them is different (non-words for _wordPrev and
// words for _wordNext) it results in the inconsistent feeling that these have compared to more modern algorithms.
// TODO: GH#15787
size_t COOKED_READ_DATA::_wordPrev(const std::wstring_view& chars, size_t position)
{
    if (position != 0)
    {
        --position;
        while (position != 0 && chars[position] == L' ')
        {
            --position;
        }

        const auto dc = DelimiterClass(chars[position]);
        while (position != 0 && DelimiterClass(chars[position - 1]) == dc)
        {
            --position;
        }
    }
    return position;
}

size_t COOKED_READ_DATA::_wordNext(const std::wstring_view& chars, size_t position)
{
    if (position < chars.size())
    {
        ++position;
        const auto dc = DelimiterClass(chars[position - 1]);
        while (position != chars.size() && dc == DelimiterClass(chars[position]))
        {
            ++position;
        }
        while (position != chars.size() && chars[position] == L' ')
        {
            ++position;
        }
    }
    return position;
}

const std::wstring_view& COOKED_READ_DATA::_newlineSuffix() const noexcept
{
    static constexpr std::wstring_view cr{ L"\r" };
    static constexpr std::wstring_view crlf{ L"\r\n" };
    return WI_IsFlagSet(_pInputBuffer->InputMode, ENABLE_PROCESSED_INPUT) ? crlf : cr;
}

// Reads text off of the InputBuffer and dispatches it to the current popup or otherwise into the _buffer contents.
bool COOKED_READ_DATA::_readCharInputLoop()
{
    for (;;)
    {
        const auto hasPopup = !_popups.empty();
        auto charOrVkey = UNICODE_NULL;
        auto commandLineEditingKeys = false;
        auto popupKeys = false;
        const auto pCommandLineEditingKeys = hasPopup ? nullptr : &commandLineEditingKeys;
        const auto pPopupKeys = hasPopup ? &popupKeys : nullptr;
        DWORD modifiers = 0;

        const auto status = GetChar(_pInputBuffer, &charOrVkey, true, pCommandLineEditingKeys, pPopupKeys, &modifiers);
        if (status == CONSOLE_STATUS_WAIT)
        {
            return false;
        }
        THROW_IF_NTSTATUS_FAILED(status);

        if (hasPopup)
        {
            const auto wch = static_cast<wchar_t>(popupKeys ? 0 : charOrVkey);
            const auto vkey = static_cast<uint16_t>(popupKeys ? charOrVkey : 0);
            if (_popupHandleInput(wch, vkey, modifiers))
            {
                return true;
            }
        }
        else
        {
            if (commandLineEditingKeys)
            {
                _handleVkey(charOrVkey, modifiers);
            }
            else if (_handleChar(charOrVkey, modifiers))
            {
                return true;
            }
        }
    }
}

// Handles character input for _readCharInputLoop() when no popups exist.
bool COOKED_READ_DATA::_handleChar(wchar_t wch, const DWORD modifiers)
{
    // All paths in this function modify the buffer.

    if (_ctrlWakeupMask != 0 && wch < L' ' && (_ctrlWakeupMask & (1 << wch)))
    {
        _flushBuffer();

        // The old implementation (all the way since the 90s) overwrote the character at the current cursor position with the given wch.
        // But simultaneously it incremented the buffer length, which would have only worked if it was written at the end of the buffer.
        // Press tab past the "f" in the string "foo" and you'd get "f\to " (a trailing whitespace; the initial contents of the buffer back then).
        // It's unclear whether the original intention was to write at the end of the buffer at all times or to implement an insert mode.
        // I went with insert mode.
        _buffer.insert(_bufferCursor, 1, wch);
        _bufferCursor++;

        _controlKeyState = modifiers;
        return true;
    }

    switch (wch)
    {
    case UNICODE_CARRIAGERETURN:
    {
        _buffer.append(_newlineSuffix());
        _bufferCursor = _buffer.size();
        _markAsDirty();
        return true;
    }
    case EXTKEY_ERASE_PREV_WORD: // Ctrl+Backspace
    case UNICODE_BACKSPACE:
        if (WI_IsFlagSet(_pInputBuffer->InputMode, ENABLE_PROCESSED_INPUT))
        {
            size_t pos;
            if (wch == EXTKEY_ERASE_PREV_WORD)
            {
                pos = _wordPrev(_buffer, _bufferCursor);
            }
            else
            {
                pos = TextBuffer::GraphemePrev(_buffer, _bufferCursor);
            }

            _buffer.erase(pos, _bufferCursor - pos);
            _bufferCursor = pos;
            _markAsDirty();

            // Notify accessibility to read the backspaced character.
            // See GH:12735, MSFT:31748387
            if (_screenInfo.HasAccessibilityEventing())
            {
                if (const auto pConsoleWindow = ServiceLocator::LocateConsoleWindow())
                {
                    LOG_IF_FAILED(pConsoleWindow->SignalUia(UIA_Text_TextChangedEventId));
                }
            }
            return false;
        }
        // If processed mode is disabled, control characters like backspace are treated like any other character.
        break;
    default:
        break;
    }

    if (_insertMode)
    {
        _buffer.insert(_bufferCursor, 1, wch);
    }
    else
    {
        // TODO GH#15875: If the input grapheme is >1 char, then this will replace >1 grapheme
        // --> We should accumulate input text as much as possible and then call _processInput with wstring_view.
        const auto nextGraphemeLength = TextBuffer::GraphemeNext(_buffer, _bufferCursor) - _bufferCursor;
        _buffer.replace(_bufferCursor, nextGraphemeLength, 1, wch);
    }

    _bufferCursor++;
    _markAsDirty();
    return false;
}

// Handles non-character input for _readCharInputLoop() when no popups exist.
void COOKED_READ_DATA::_handleVkey(uint16_t vkey, DWORD modifiers)
{
    const auto ctrlPressed = WI_IsAnyFlagSet(modifiers, LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
    const auto altPressed = WI_IsAnyFlagSet(modifiers, LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);

    switch (vkey)
    {
    case VK_ESCAPE:
        if (!_buffer.empty())
        {
            _buffer.clear();
            _bufferCursor = 0;
            _markAsDirty();
        }
        break;
    case VK_HOME:
        if (_bufferCursor > 0)
        {
            if (ctrlPressed)
            {
                _buffer.erase(0, _bufferCursor);
            }
            _bufferCursor = 0;
            _markAsDirty();
        }
        break;
    case VK_END:
        if (_bufferCursor < _buffer.size())
        {
            if (ctrlPressed)
            {
                _buffer.erase(_bufferCursor);
            }
            _bufferCursor = _buffer.size();
            _markAsDirty();
        }
        break;
    case VK_LEFT:
        if (_bufferCursor != 0)
        {
            if (ctrlPressed)
            {
                _bufferCursor = _wordPrev(_buffer, _bufferCursor);
            }
            else
            {
                _bufferCursor = TextBuffer::GraphemePrev(_buffer, _bufferCursor);
            }
            _markAsDirty();
        }
        break;
    case VK_F1:
    case VK_RIGHT:
        if (_bufferCursor != _buffer.size())
        {
            if (ctrlPressed && vkey == VK_RIGHT)
            {
                _bufferCursor = _wordNext(_buffer, _bufferCursor);
            }
            else
            {
                _bufferCursor = TextBuffer::GraphemeNext(_buffer, _bufferCursor);
            }
            _markAsDirty();
        }
        else if (_history)
        {
            // Traditionally pressing right at the end of an input line would paste characters from the previous command.
            const auto cmd = _history->GetLastCommand();
            const auto bufferSize = _buffer.size();
            const auto cmdSize = cmd.size();
            size_t bufferBeg = 0;
            size_t cmdBeg = 0;

            // We cannot just check if the cmd is longer than the _buffer, because we want to copy graphemes,
            // not characters and there's no correlation between the number of graphemes and their byte length.
            while (cmdBeg < cmdSize)
            {
                const auto cmdEnd = TextBuffer::GraphemeNext(cmd, cmdBeg);

                if (bufferBeg >= bufferSize)
                {
                    _buffer.append(cmd, cmdBeg, cmdEnd - cmdBeg);
                    _bufferCursor = _buffer.size();
                    _markAsDirty();
                    break;
                }

                bufferBeg = TextBuffer::GraphemeNext(_buffer, bufferBeg);
                cmdBeg = cmdEnd;
            }
        }
        break;
    case VK_INSERT:
        _insertMode = !_insertMode;
        _screenInfo.SetCursorDBMode(_insertMode != ServiceLocator::LocateGlobals().getConsoleInformation().GetInsertMode());
        _markAsDirty();
        break;
    case VK_DELETE:
        if (_bufferCursor < _buffer.size())
        {
            _buffer.erase(_bufferCursor, TextBuffer::GraphemeNext(_buffer, _bufferCursor) - _bufferCursor);
            _markAsDirty();
        }
        break;
    case VK_UP:
    case VK_F5:
        if (_history && !_history->AtFirstCommand())
        {
            _replaceBuffer(_history->Retrieve(CommandHistory::SearchDirection::Previous));
        }
        break;
    case VK_DOWN:
        if (_history && !_history->AtLastCommand())
        {
            _replaceBuffer(_history->Retrieve(CommandHistory::SearchDirection::Next));
        }
        break;
    case VK_PRIOR:
        if (_history && !_history->AtFirstCommand())
        {
            _replaceBuffer(_history->RetrieveNth(0));
        }
        break;
    case VK_NEXT:
        if (_history && !_history->AtLastCommand())
        {
            _replaceBuffer(_history->RetrieveNth(INT_MAX));
        }
        break;
    case VK_F2:
        if (_history)
        {
            _popupPush(PopupKind::CopyToChar);
        }
        break;
    case VK_F3:
        if (_history)
        {
            const auto last = _history->GetLastCommand();
            if (last.size() > _bufferCursor)
            {
                const auto count = last.size() - _bufferCursor;
                _buffer.replace(_bufferCursor, count, last, _bufferCursor, count);
                _bufferCursor += count;
                _markAsDirty();
            }
        }
        break;
    case VK_F4:
        // Historically the CopyFromChar popup was constrained to only work when a history exists,
        // but I don't see why that should be. It doesn't depend on _history at all.
        _popupPush(PopupKind::CopyFromChar);
        break;
    case VK_F6:
        // Don't ask me why but F6 is an alias for ^Z.
        _handleChar(0x1a, modifiers);
        break;
    case VK_F7:
        if (!ctrlPressed && !altPressed)
        {
            if (_history && _history->GetNumberOfCommands())
            {
                _popupPush(PopupKind::CommandList);
            }
        }
        else if (altPressed)
        {
            if (_history)
            {
                _history->Empty();
                _history->Flags |= CommandHistory::CLE_ALLOCATED;
            }
        }
        break;
    case VK_F8:
        if (_history)
        {
            CommandHistory::Index index = 0;
            const auto prefix = std::wstring_view{ _buffer }.substr(0, _bufferCursor);
            if (_history->FindMatchingCommand(prefix, _history->LastDisplayed, index, CommandHistory::MatchOptions::None))
            {
                _buffer.assign(_history->RetrieveNth(index));
                _bufferCursor = std::min(_bufferCursor, _buffer.size());
                _markAsDirty();
            }
        }
        break;
    case VK_F9:
        if (_history && _history->GetNumberOfCommands())
        {
            _popupPush(PopupKind::CommandNumber);
        }
        break;
    case VK_F10:
        // Alt+F10 clears the aliases for specifically cmd.exe.
        if (altPressed)
        {
            Alias::s_ClearCmdExeAliases();
        }
        break;
    default:
        assert(false); // Unrecognized VK. Fix or don't call this function?
        break;
    }
}

// Handles any tasks that need to be completed after the read input loop finishes,
// like handling doskey aliases and converting the input to non-UTF16.
void COOKED_READ_DATA::_handlePostCharInputLoop(const bool isUnicode, size_t& numBytes, ULONG& controlKeyState)
{
    auto writer = _userBuffer;
    std::wstring_view input{ _buffer };
    size_t lineCount = 1;

    if (WI_IsFlagSet(_pInputBuffer->InputMode, ENABLE_ECHO_INPUT))
    {
        // The last characters in line-read are a \r or \r\n unless _ctrlWakeupMask was used.
        // Neither History nor s_MatchAndCopyAlias want to know about them.
        const auto& suffix = _newlineSuffix();
        if (input.ends_with(suffix))
        {
            input.remove_suffix(suffix.size());

            if (_history)
            {
                auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
                LOG_IF_FAILED(_history->Add(input, WI_IsFlagSet(gci.Flags, CONSOLE_HISTORY_NODUP)));
            }

            Tracing::s_TraceCookedRead(_processHandle, input);

            const auto alias = Alias::s_MatchAndCopyAlias(input, _exeName, lineCount);
            if (!alias.empty())
            {
                _buffer = alias;
            }

            // NOTE: Even if there's no alias we should restore the trailing \r\n that we removed above.
            input = std::wstring_view{ _buffer };

            // doskey aliases may result in multiple lines of output (for instance `doskey test=echo foo$Techo bar$Techo baz`).
            // We need to emit them as multiple cooked reads as well, so that each read completes at a \r\n.
            if (lineCount > 1)
            {
                // ProcessAliases() is supposed to end each line with \r\n. If it doesn't we might as well fail-fast.
                const auto firstLineEnd = input.find(UNICODE_LINEFEED) + 1;
                input = input.substr(0, std::min(input.size(), firstLineEnd));
            }
        }
    }

    const auto inputSizeBefore = input.size();
    _pInputBuffer->Consume(isUnicode, input, writer);

    if (lineCount > 1)
    {
        // This is a continuation of the above identical if condition.
        // We've truncated the `input` slice and now we need to restore it.
        const auto inputSizeAfter = input.size();
        const auto amountConsumed = inputSizeBefore - inputSizeAfter;
        input = std::wstring_view{ _buffer };
        input = input.substr(std::min(input.size(), amountConsumed));
        GetInputReadHandleData()->SaveMultilinePendingInput(input);
    }
    else if (!input.empty())
    {
        GetInputReadHandleData()->SavePendingInput(input);
    }

    auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    gci.Flags |= CONSOLE_IGNORE_NEXT_KEYUP;

    // If we previously called SetCursorDBMode() with true,
    // this will ensure that the cursor returns to its normal look.
    _screenInfo.SetCursorDBMode(false);

    numBytes = _userBuffer.size() - writer.size();
    controlKeyState = _controlKeyState;
}

// Signals to _flushBuffer() that the contents of _buffer are stale and need to be redrawn.
// ALL _buffer and _bufferCursor changes must be flagged with _markAsDirty().
//
// By using _bufferDirty to avoid redrawing the buffer unless needed, we turn the amortized time complexity of _readCharInputLoop()
// from O(n^2) (n(n+1)/2 redraws) into O(n). Pasting text would quickly turn into "accidentally quadratic" meme material otherwise.
void COOKED_READ_DATA::_markAsDirty()
{
    _bufferDirty = true;
}

// Draws the contents of _buffer onto the screen.
void COOKED_READ_DATA::_flushBuffer()
{
    // _flushBuffer() is called often and is a good place to assert() that our _bufferCursor is still in bounds.
    assert(_bufferCursor <= _buffer.size());
    _bufferCursor = std::min(_bufferCursor, _buffer.size());

    if (!_bufferDirty)
    {
        return;
    }

    if (WI_IsFlagSet(_pInputBuffer->InputMode, ENABLE_ECHO_INPUT))
    {
        _unwindCursorPosition(_distanceCursor);

        const std::wstring_view view{ _buffer };
        const auto distanceBeforeCursor = _writeChars(view.substr(0, _bufferCursor));
        const auto distanceAfterCursor = _writeChars(view.substr(_bufferCursor));
        const auto distanceEnd = distanceBeforeCursor + distanceAfterCursor;
        const auto eraseDistance = std::max(0, _distanceEnd - distanceEnd);

        // If the contents of _buffer became shorter we'll have to erase the previously printed contents.
        _erase(eraseDistance);
        _unwindCursorPosition(distanceAfterCursor + eraseDistance);

        _distanceCursor = distanceBeforeCursor;
        _distanceEnd = distanceEnd;
    }

    _bufferDirty = false;
}

// This is just a small helper to fill the next N cells starting at the current cursor position with whitespace.
// The implementation is inefficient for `count`s larger than 7, but such calls are uncommon to happen (namely only when resizing the window).
void COOKED_READ_DATA::_erase(const til::CoordType distance)
{
    if (distance > 0)
    {
        const std::wstring str(gsl::narrow_cast<size_t>(distance), L' ');
        std::ignore = _writeChars(str);
    }
}

// A helper to write text and calculate the number of cells we've written.
// _unwindCursorPosition then allows us to go that many cells back. Tracking cells instead of explicit
// buffer positions allows us to pay no further mind to whether the buffer scrolled up or not.
til::CoordType COOKED_READ_DATA::_writeChars(const std::wstring_view& text) const
{
    if (text.empty())
    {
        return 0;
    }

    const auto& textBuffer = _screenInfo.GetTextBuffer();
    const auto& cursor = textBuffer.GetCursor();
    const auto width = textBuffer.GetSize().Width();
    const auto initialCursorPos = cursor.GetPosition();
    til::CoordType scrollY = 0;

    WriteCharsLegacy(_screenInfo, text, true, &scrollY);

    const auto finalCursorPos = cursor.GetPosition();
    return (finalCursorPos.y - initialCursorPos.y + scrollY) * width + finalCursorPos.x - initialCursorPos.x;
}

// Moves the given point by the given distance inside the text buffer, as if moving a cursor with the left/right arrow keys.
til::point COOKED_READ_DATA::_offsetPosition(til::point pos, til::CoordType distance) const
{
    const auto size = _screenInfo.GetTextBuffer().GetSize().Dimensions();
    const auto w = static_cast<ptrdiff_t>(size.width);
    const auto h = static_cast<ptrdiff_t>(size.height);
    const auto area = w * h;

    auto off = w * pos.y + pos.x;
    off += distance;
    off = off < 0 ? 0 : (off > area ? area : off);

    return {
        gsl::narrow_cast<til::CoordType>(off % w),
        gsl::narrow_cast<til::CoordType>(off / w),
    };
}

// This moves the cursor `distance`-many cells back up in the buffer.
// It's intended to be used in combination with _writeChars.
void COOKED_READ_DATA::_unwindCursorPosition(til::CoordType distance) const
{
    if (distance <= 0)
    {
        // If all the code in this file works correctly, negative distances should not occur.
        // If they do occur it would indicate a bug that would need to be fixed urgently.
        assert(distance == 0);
        return;
    }

    const auto& textBuffer = _screenInfo.GetTextBuffer();
    const auto& cursor = textBuffer.GetCursor();
    const auto pos = _offsetPosition(cursor.GetPosition(), -distance);

    std::ignore = _screenInfo.SetCursorPosition(pos, true);
    _screenInfo.MakeCursorVisible(pos);
}

// Just a simple helper to replace the entire buffer contents.
void COOKED_READ_DATA::_replaceBuffer(const std::wstring_view& str)
{
    _buffer.assign(str);
    _bufferCursor = _buffer.size();
    _markAsDirty();
}

// If the viewport is large enough to fit a popup, this function prepares everything for
// showing the given type. It handles computing the size of the popup, its position,
// backs the affected area up and draws the border and initial contents.
void COOKED_READ_DATA::_popupPush(const PopupKind kind)
try
{
    auto& textBuffer = _screenInfo.GetTextBuffer();
    const auto viewport = _screenInfo.GetViewport();
    const auto viewportOrigin = viewport.Origin();
    const auto viewportSize = viewport.Dimensions();

    til::size proposedSize;
    switch (kind)
    {
    case PopupKind::CopyToChar:
        proposedSize = { 26, 1 };
        break;
    case PopupKind::CopyFromChar:
        proposedSize = { 28, 1 };
        break;
    case PopupKind::CommandNumber:
        proposedSize = { 22 + CommandNumberMaxInputLength, 1 };
        break;
    case PopupKind::CommandList:
    {
        const auto& commands = _history->GetCommands();
        const auto commandCount = _history->GetNumberOfCommands();

        size_t maxStringLength = 0;
        for (const auto& c : commands)
        {
            maxStringLength = std::max(maxStringLength, c.size());
        }

        // Account for the "123: " prefix each line gets.
        maxStringLength += integerLog10(commandCount);
        maxStringLength += 3;

        // conhost used to draw the command list with a size of 40x10, but at some point it switched over to dynamically
        // sizing it depending on the history count and width of the entries. Back then it was implemented with the
        // assumption that the code unit count equals the column count, which I kept because I don't want the TextBuffer
        // class to expose how wide characters are, any more than necessary. It makes implementing Unicode support
        // much harder, because things like combining marks and work zones may cause TextBuffer to end up deciding
        // a piece of text has a different size than what you thought it had when measuring it on its own.
        proposedSize.width = gsl::narrow_cast<til::CoordType>(std::clamp<size_t>(maxStringLength, 40, til::CoordTypeMax));
        proposedSize.height = std::clamp(commandCount, 10, 20);
        break;
    }
    default:
        assert(false);
        return;
    }

    // Subtract 2 because we need to draw the border around our content. We must return early if we're
    // unable to do so, otherwise the remaining code fails because the size would be zero/negative.
    const til::size contentSize{
        std::min(proposedSize.width, viewportSize.width - 2),
        std::min(proposedSize.height, viewportSize.height - 2),
    };
    if (!contentSize)
    {
        return;
    }

    const auto widthSizeT = gsl::narrow_cast<size_t>(contentSize.width + 2);
    const auto heightSizeT = gsl::narrow_cast<size_t>(contentSize.height + 2);
    const til::point contentOrigin{
        (viewportSize.width - contentSize.width) / 2 + viewportOrigin.x,
        (viewportSize.height - contentSize.height) / 2 + viewportOrigin.y,
    };
    const til::rect contentRect{
        contentOrigin,
        contentSize,
    };
    const auto backupRect = Viewport::FromExclusive({
        contentRect.left - 1,
        contentRect.top - 1,
        contentRect.right + 1,
        contentRect.bottom + 1,
    });

    auto& popup = _popups.emplace_back(kind, contentRect, backupRect, std::vector<CHAR_INFO>{ widthSizeT * heightSizeT });

    // Create a backup of the TextBuffer parts we're scribbling over.
    // We need to flush the buffer to ensure we capture the latest contents.
    // NOTE: This may theoretically modify popup.backupRect (practically unlikely).
    _flushBuffer();
    THROW_IF_FAILED(ServiceLocator::LocateGlobals().api->ReadConsoleOutputWImpl(_screenInfo, popup.backup, backupRect, popup.backupRect));

    // Draw the border around our content and fill it with whitespace to prepare it for future usage.
    {
        const auto attributes = _screenInfo.GetPopupAttributes();

        RowWriteState state{
            .columnBegin = contentRect.left - 1,
            .columnLimit = contentRect.right + 1,
        };

        // top line ┌───┐
        std::wstring buffer;
        buffer.assign(widthSizeT, L'─');
        buffer.front() = L'┌';
        buffer.back() = L'┐';
        state.text = buffer;
        textBuffer.Write(contentRect.top - 1, attributes, state);

        // bottom line └───┘
        buffer.front() = L'└';
        buffer.back() = L'┘';
        state.text = buffer;
        textBuffer.Write(contentRect.bottom, attributes, state);

        // middle lines │   │
        buffer.assign(widthSizeT, L' ');
        buffer.front() = L'│';
        buffer.back() = L'│';
        for (til::CoordType y = contentRect.top; y < contentRect.bottom; ++y)
        {
            state.text = buffer;
            textBuffer.Write(y, attributes, state);
        }
    }

    switch (kind)
    {
    case PopupKind::CopyToChar:
        _popupDrawPrompt(popup, ID_CONSOLE_MSGCMDLINEF2);
        break;
    case PopupKind::CopyFromChar:
        _popupDrawPrompt(popup, ID_CONSOLE_MSGCMDLINEF4);
        break;
    case PopupKind::CommandNumber:
        popup.commandNumber.buffer.fill(' ');
        popup.commandNumber.bufferSize = 0;
        _popupDrawPrompt(popup, ID_CONSOLE_MSGCMDLINEF9);
        break;
    case PopupKind::CommandList:
        popup.commandList.selected = _history->LastDisplayed;
        popup.commandList.top = popup.commandList.selected - contentSize.height / 2;
        _popupDrawCommandList(popup);
        break;
    default:
        assert(false);
    }

    // If this is the first popup to be shown, stop the cursor from appearing/blinking
    if (_popups.size() == 1)
    {
        textBuffer.GetCursor().SetIsPopupShown(true);
    }
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    // Using _popupsDone() is a convenient way to restore the buffer contents if anything in this call failed.
    // This could technically dismiss an unrelated popup that was already in _popups, but reaching this point is unlikely anyways.
    _popupsDone();
}

// Dismisses all current popups at once. Right now we don't need support for just dismissing the topmost popup.
// In fact, there's only a single situation right now where there can be >1 popup:
// Pressing F7 followed by F9 (CommandNumber on top of CommandList).
void COOKED_READ_DATA::_popupsDone()
{
    while (!_popups.empty())
    {
        auto& popup = _popups.back();

        // Restore TextBuffer contents. They could be empty if _popupPush()
        // threw an exception in the middle of construction.
        if (!popup.backup.empty())
        {
            [[maybe_unused]] Viewport unused;
            LOG_IF_FAILED(ServiceLocator::LocateGlobals().api->WriteConsoleOutputWImpl(_screenInfo, popup.backup, popup.backupRect, unused));
        }

        _popups.pop_back();
    }

    // Restore cursor blinking.
    _screenInfo.GetTextBuffer().GetCursor().SetIsPopupShown(false);
}

bool COOKED_READ_DATA::_popupHandleInput(wchar_t wch, uint16_t vkey, DWORD modifiers)
{
    if (_popups.empty())
    {
        assert(false); // Don't call this function.
        return false;
    }

    auto& popup = _popups.back();
    switch (popup.kind)
    {
    case PopupKind::CopyToChar:
        _popupHandleCopyToCharInput(popup, wch, vkey, modifiers);
        return false;
    case PopupKind::CopyFromChar:
        _popupHandleCopyFromCharInput(popup, wch, vkey, modifiers);
        return false;
    case PopupKind::CommandNumber:
        _popupHandleCommandNumberInput(popup, wch, vkey, modifiers);
        return false;
    case PopupKind::CommandList:
        return _popupHandleCommandListInput(popup, wch, vkey, modifiers);
    default:
        return false;
    }
}

void COOKED_READ_DATA::_popupHandleCopyToCharInput(Popup& /*popup*/, const wchar_t wch, const uint16_t vkey, const DWORD /*modifiers*/)
{
    if (vkey)
    {
        if (vkey == VK_ESCAPE)
        {
            _popupsDone();
        }
    }
    else
    {
        // See PopupKind::CopyToChar for more information about this code.
        const auto cmd = _history->GetLastCommand();
        const auto idx = cmd.find(wch, _bufferCursor);

        if (idx != decltype(cmd)::npos)
        {
            // When we enter this if condition it's guaranteed that _bufferCursor must be
            // smaller than idx, which in turn implies that it's smaller than cmd.size().
            // As such, calculating length is safe and str.size() == length.
            const auto count = idx - _bufferCursor;
            _buffer.replace(_bufferCursor, count, cmd, _bufferCursor, count);
            _bufferCursor += count;
            _markAsDirty();
        }

        _popupsDone();
    }
}

void COOKED_READ_DATA::_popupHandleCopyFromCharInput(Popup& /*popup*/, const wchar_t wch, const uint16_t vkey, const DWORD /*modifiers*/)
{
    if (vkey)
    {
        if (vkey == VK_ESCAPE)
        {
            _popupsDone();
        }
    }
    else
    {
        // See PopupKind::CopyFromChar for more information about this code.
        const auto idx = _buffer.find(wch, _bufferCursor);
        _buffer.erase(_bufferCursor, std::min(idx, _buffer.size()) - _bufferCursor);
        _markAsDirty();
        _popupsDone();
    }
}

void COOKED_READ_DATA::_popupHandleCommandNumberInput(Popup& popup, const wchar_t wch, const uint16_t vkey, const DWORD /*modifiers*/)
{
    if (vkey)
    {
        if (vkey == VK_ESCAPE)
        {
            _popupsDone();
        }
    }
    else
    {
        if (wch == UNICODE_CARRIAGERETURN)
        {
            popup.commandNumber.buffer[popup.commandNumber.bufferSize++] = L'\0';
            _replaceBuffer(_history->RetrieveNth(std::stoi(popup.commandNumber.buffer.data())));
            _popupsDone();
            return;
        }

        if (wch >= L'0' && wch <= L'9')
        {
            if (popup.commandNumber.bufferSize < CommandNumberMaxInputLength)
            {
                popup.commandNumber.buffer[popup.commandNumber.bufferSize++] = wch;
            }
        }
        else if (wch == UNICODE_BACKSPACE)
        {
            if (popup.commandNumber.bufferSize > 0)
            {
                popup.commandNumber.buffer[--popup.commandNumber.bufferSize] = L' ';
            }
        }
        else
        {
            return;
        }

        RowWriteState state{
            .text = { popup.commandNumber.buffer.data(), CommandNumberMaxInputLength },
            .columnBegin = popup.contentRect.right - CommandNumberMaxInputLength,
            .columnLimit = popup.contentRect.right,
        };
        _screenInfo.GetTextBuffer().Write(popup.contentRect.top, _screenInfo.GetPopupAttributes(), state);
    }
}

bool COOKED_READ_DATA::_popupHandleCommandListInput(Popup& popup, const wchar_t wch, const uint16_t vkey, const DWORD modifiers)
{
    auto& cl = popup.commandList;

    if (wch == UNICODE_CARRIAGERETURN)
    {
        _buffer.assign(_history->RetrieveNth(cl.selected));
        _popupsDone();
        return _handleChar(UNICODE_CARRIAGERETURN, modifiers);
    }

    switch (vkey)
    {
    case VK_ESCAPE:
        _popupsDone();
        return false;
    case VK_F9:
        _popupPush(PopupKind::CommandNumber);
        return false;
    case VK_DELETE:
        _history->Remove(cl.selected);
        if (_history->GetNumberOfCommands() <= 0)
        {
            _popupsDone();
            return false;
        }
        break;
    case VK_LEFT:
    case VK_RIGHT:
        _replaceBuffer(_history->RetrieveNth(cl.selected));
        _popupsDone();
        return false;
    case VK_UP:
        if (WI_IsFlagSet(modifiers, SHIFT_PRESSED))
        {
            _history->Swap(cl.selected, cl.selected - 1);
        }
        // _popupDrawCommandList() clamps all values to valid ranges in `cl`.
        cl.selected--;
        break;
    case VK_DOWN:
        if (WI_IsFlagSet(modifiers, SHIFT_PRESSED))
        {
            _history->Swap(cl.selected, cl.selected + 1);
        }
        // _popupDrawCommandList() clamps all values to valid ranges in `cl`.
        cl.selected++;
        break;
    case VK_HOME:
        cl.selected = 0;
        break;
    case VK_END:
        // _popupDrawCommandList() clamps all values to valid ranges in `cl`.
        cl.selected = INT_MAX;
        break;
    case VK_PRIOR:
        // _popupDrawCommandList() clamps all values to valid ranges in `cl`.
        cl.selected -= popup.contentRect.height();
        break;
    case VK_NEXT:
        // _popupDrawCommandList() clamps all values to valid ranges in `cl`.
        cl.selected += popup.contentRect.height();
        break;
    default:
        return false;
    }

    _popupDrawCommandList(popup);
    return false;
}

void COOKED_READ_DATA::_popupDrawPrompt(const Popup& popup, const UINT id) const
{
    const auto text = _LoadString(id);
    RowWriteState state{
        .text = text,
        .columnBegin = popup.contentRect.left,
        .columnLimit = popup.contentRect.right,
    };
    _screenInfo.GetTextBuffer().Write(popup.contentRect.top, _screenInfo.GetPopupAttributes(), state);
}

void COOKED_READ_DATA::_popupDrawCommandList(Popup& popup) const
{
    assert(popup.kind == PopupKind::CommandList);

    auto& cl = popup.commandList;
    const auto max = _history->GetNumberOfCommands();
    const auto width = popup.contentRect.narrow_width<size_t>();
    const auto height = std::min(popup.contentRect.height(), _history->GetNumberOfCommands());
    const auto dirtyHeight = std::max(height, cl.dirtyHeight);

    {
        // The viewport movement of the popup is anchored around the current selection first and foremost.
        cl.selected = std::clamp(cl.selected, 0, max - 1);

        // It then lazily follows it when the selection goes out of the viewport.
        if (cl.selected < cl.top)
        {
            cl.top = cl.selected;
        }
        else if (cl.selected >= cl.top + height)
        {
            cl.top = cl.selected - height + 1;
        }

        cl.top = std::clamp(cl.top, 0, max - height);
    }

    std::wstring buffer;
    buffer.reserve(width * 2 + 4);

    const auto& attrRegular = _screenInfo.GetPopupAttributes();
    auto attrInverted = attrRegular;
    attrInverted.Invert();

    RowWriteState state{
        .columnBegin = popup.contentRect.left,
        .columnLimit = popup.contentRect.right,
    };

    for (til::CoordType off = 0; off < dirtyHeight; ++off)
    {
        const auto y = popup.contentRect.top + off;
        const auto historyIndex = cl.top + off;
        const auto str = _history->GetNth(historyIndex);
        const auto& attr = historyIndex == cl.selected ? attrInverted : attrRegular;

        buffer.clear();
        if (!str.empty())
        {
            buffer.append(std::to_wstring(historyIndex));
            buffer.append(L": ");
            buffer.append(str);
        }
        buffer.append(width, L' ');

        state.text = buffer;
        _screenInfo.GetTextBuffer().Write(y, attr, state);
    }

    cl.dirtyHeight = height;
}
