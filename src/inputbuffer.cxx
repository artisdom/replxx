#include <memory>

#ifdef _WIN32

#include <conio.h>
#include <windows.h>
#include <io.h>
#if _MSC_VER < 1900
#define snprintf _snprintf	// Microsoft headers use underscores in some names
#endif
#define strcasecmp _stricmp
#define strdup _strdup
#define isatty _isatty
#define write _write
#define STDIN_FILENO 0

#else /* _WIN32 */

#include <unistd.h>
#include <signal.h>

#endif /* _WIN32 */

#include "inputbuffer.hxx"
#include "prompt.hxx"
#include "util.hxx"
#include "io.hxx"
#include "setup.hxx"
#include "keycodes.hxx"
#include "killring.hxx"
#include "history.hxx"
#include "replxx.hxx"

using namespace std;

namespace replxx {

#ifndef _WIN32
extern bool gotResize;
#endif
static KillRing killRing;

/**
 * Free memory used in a recent command completion session
 *
 * @param lc pointer to a replxx_completions struct
 */
void freeCompletions(replxx_completions* lc) {
	lc->completionStrings.clear();
}

void InputBuffer::preloadBuffer(const char* preloadText) {
	size_t ucharCount = 0;
	copyString8to32(buf32, buflen + 1, ucharCount, preloadText);
	recomputeCharacterWidths(buf32, charWidths, static_cast<int>(ucharCount));
	len = static_cast<int>(ucharCount);
	pos = static_cast<int>(ucharCount);
}

/**
 * Refresh the user's input line: the prompt is already onscreen and is not
 * redrawn here
 * @param pi	 PromptBase struct holding information about the prompt and our
 * screen position
 */
void InputBuffer::refreshLine(PromptBase& pi) {
	// check for a matching brace/bracket/paren, remember its position if found
	int highlight = -1;
	if (pos < len) {
		/* this scans for a brace matching buf32[pos] to highlight */
		int scanDirection = 0;
		if (strchr("}])", buf32[pos]))
			scanDirection = -1; /* backwards */
		else if (strchr("{[(", buf32[pos]))
			scanDirection = 1; /* forwards */

		if (scanDirection) {
			int unmatched = scanDirection;
			for (int i = pos + scanDirection; i >= 0 && i < len; i += scanDirection) {
				/* TODO: the right thing when inside a string */
				if (strchr("}])", buf32[i]))
					--unmatched;
				else if (strchr("{[(", buf32[i]))
					++unmatched;

				if (unmatched == 0) {
					highlight = i;
					break;
				}
			}
		}
	}

	// calculate the position of the end of the input line
	int xEndOfInput, yEndOfInput;
	calculateScreenPosition(pi.promptIndentation, 0, pi.promptScreenColumns,
													calculateColumnPosition(buf32, len), xEndOfInput,
													yEndOfInput);

	// calculate the desired position of the cursor
	int xCursorPos, yCursorPos;
	calculateScreenPosition(pi.promptIndentation, 0, pi.promptScreenColumns,
													calculateColumnPosition(buf32, pos), xCursorPos,
													yCursorPos);

#ifdef _WIN32
	// position at the end of the prompt, clear to end of previous input
	CONSOLE_SCREEN_BUFFER_INFO inf;
	GetConsoleScreenBufferInfo(console_out, &inf);
	inf.dwCursorPosition.X = pi.promptIndentation;	// 0-based on Win32
	inf.dwCursorPosition.Y -= pi.promptCursorRowOffset - pi.promptExtraLines;
	SetConsoleCursorPosition(console_out, inf.dwCursorPosition);
	DWORD count;
	if (len < pi.promptPreviousInputLen)
		FillConsoleOutputCharacterA(console_out, ' ', pi.promptPreviousInputLen,
																inf.dwCursorPosition, &count);
	pi.promptPreviousInputLen = len;

	// display the input line
	if (highlight == -1) {
		if (write32(1, buf32, len) == -1) return;
	} else {
		if (write32(1, buf32, highlight) == -1) return;
		setDisplayAttribute(true); /* bright blue (visible with both B&W bg) */
		if (write32(1, &buf32[highlight], 1) == -1) return;
		setDisplayAttribute(false);
		if (write32(1, buf32 + highlight + 1, len - highlight - 1) == -1) return;
	}

	// position the cursor
	GetConsoleScreenBufferInfo(console_out, &inf);
	inf.dwCursorPosition.X = xCursorPos;	// 0-based on Win32
	inf.dwCursorPosition.Y -= yEndOfInput - yCursorPos;
	SetConsoleCursorPosition(console_out, inf.dwCursorPosition);
#else	// _WIN32
	char seq[64];
	int cursorRowMovement = pi.promptCursorRowOffset - pi.promptExtraLines;
	if (cursorRowMovement > 0) {	// move the cursor up as required
		snprintf(seq, sizeof seq, "\x1b[%dA", cursorRowMovement);
		if (write(1, seq, strlen(seq)) == -1) return;
	}
	// position at the end of the prompt, clear to end of screen
	snprintf(seq, sizeof seq, "\x1b[%dG\x1b[J",
					 pi.promptIndentation + 1);	// 1-based on VT100
	if (write(1, seq, strlen(seq)) == -1) return;

	if (highlight == -1) {	// write unhighlighted text
		if (write32(1, buf32, len) == -1) return;
	} else {	// highlight the matching brace/bracket/parenthesis
		if (write32(1, buf32, highlight) == -1) return;
		setDisplayAttribute(true);
		if (write32(1, &buf32[highlight], 1) == -1) return;
		setDisplayAttribute(false);
		if (write32(1, buf32 + highlight + 1, len - highlight - 1) == -1) return;
	}

	// we have to generate our own newline on line wrap
	if (xEndOfInput == 0 && yEndOfInput > 0)
		if (write(1, "\n", 1) == -1) return;

	// position the cursor
	cursorRowMovement = yEndOfInput - yCursorPos;
	if (cursorRowMovement > 0) {	// move the cursor up as required
		snprintf(seq, sizeof seq, "\x1b[%dA", cursorRowMovement);
		if (write(1, seq, strlen(seq)) == -1) return;
	}
	// position the cursor within the line
	snprintf(seq, sizeof seq, "\x1b[%dG", xCursorPos + 1);	// 1-based on VT100
	if (write(1, seq, strlen(seq)) == -1) return;
#endif

	pi.promptCursorRowOffset =
			pi.promptExtraLines + yCursorPos;	// remember row for next pass
}

/**
 * Handle command completion, using a completionCallback() routine to provide
 * possible substitutions
 * This routine handles the mechanics of updating the user's input buffer with
 * possible replacement
 * of text as the user selects a proposed completion string, or cancels the
 * completion attempt.
 * @param pi		 PromptBase struct holding information about the prompt and our
 * screen position
 */
int InputBuffer::completeLine(PromptBase& pi) {
	replxx_completions lc;
	char32_t c = 0;

	// completionCallback() expects a parsable entity, so find the previous break
	// character and
	// extract a copy to parse.	we also handle the case where tab is hit while
	// not at end-of-line.
	int startIndex = pos;
	while (--startIndex >= 0) {
		if (strchr(setup.breakChars, buf32[startIndex])) {
			break;
		}
	}
	++startIndex;
	int itemLength = pos - startIndex;
	Utf32String unicodeCopy(&buf32[startIndex], itemLength);
	Utf8String parseItem(unicodeCopy);

	// get a list of completions
	setup.completionCallback(parseItem.get(), &lc);

	// if no completions, we are done
	if (lc.completionStrings.size() == 0) {
		beep();
		freeCompletions(&lc);
		return 0;
	}

	// at least one completion
	int longestCommonPrefix = 0;
	int displayLength = 0;
	if (lc.completionStrings.size() == 1) {
		longestCommonPrefix = static_cast<int>(lc.completionStrings[0].length());
	} else {
		bool keepGoing = true;
		while (keepGoing) {
			for (size_t j = 0; j < lc.completionStrings.size() - 1; ++j) {
				char32_t c1 = lc.completionStrings[j][longestCommonPrefix];
				char32_t c2 = lc.completionStrings[j + 1][longestCommonPrefix];
				if ((0 == c1) || (0 == c2) || (c1 != c2)) {
					keepGoing = false;
					break;
				}
			}
			if (keepGoing) {
				++longestCommonPrefix;
			}
		}
	}
	if (lc.completionStrings.size() != 1) {	// beep if ambiguous
		beep();
	}

	// if we can extend the item, extend it and return to main loop
	if (longestCommonPrefix > itemLength) {
		displayLength = len + longestCommonPrefix - itemLength;
		if (displayLength > buflen) {
			longestCommonPrefix -= displayLength - buflen;	// don't overflow buffer
			displayLength = buflen;												 // truncate the insertion
			beep();																				 // and make a noise
		}
		Utf32String displayText(displayLength + 1);
		memcpy(displayText.get(), buf32, sizeof(char32_t) * startIndex);
		memcpy(&displayText[startIndex], &lc.completionStrings[0][0],
					 sizeof(char32_t) * longestCommonPrefix);
		int tailIndex = startIndex + longestCommonPrefix;
		memcpy(&displayText[tailIndex], &buf32[pos],
					 sizeof(char32_t) * (displayLength - tailIndex + 1));
		copyString32(buf32, displayText.get(), displayLength);
		pos = startIndex + longestCommonPrefix;
		len = displayLength;
		refreshLine(pi);
		return 0;
	}

	// we can't complete any further, wait for second tab
	do {
		c = read_char();
		c = cleanupCtrl(c);
	} while (c == static_cast<char32_t>(-1));

	// if any character other than tab, pass it to the main loop
	if (c != ctrlChar('I')) {
		freeCompletions(&lc);
		return c;
	}

	// we got a second tab, maybe show list of possible completions
	bool showCompletions = true;
	bool onNewLine = false;
	if (static_cast<int>( lc.completionStrings.size() ) > setup.completionCountCutoff) {
		int savePos =
				pos;	// move cursor to EOL to avoid overwriting the command line
		pos = len;
		refreshLine(pi);
		pos = savePos;
		printf("\nDisplay all %u possibilities? (y or n)",
					 static_cast<unsigned int>(lc.completionStrings.size()));
		fflush(stdout);
		onNewLine = true;
		while (c != 'y' && c != 'Y' && c != 'n' && c != 'N' && c != ctrlChar('C')) {
			do {
				c = read_char();
				c = cleanupCtrl(c);
			} while (c == static_cast<char32_t>(-1));
		}
		switch (c) {
			case 'n':
			case 'N':
				showCompletions = false;
				freeCompletions(&lc);
				break;
			case ctrlChar('C'):
				showCompletions = false;
				freeCompletions(&lc);
				if (write(1, "^C", 2) == -1) return -1;	// Display the ^C we got
				c = 0;
				break;
		}
	}

	// if showing the list, do it the way readline does it
	bool stopList = false;
	if (showCompletions) {
		int longestCompletion = 0;
		for (size_t j = 0; j < lc.completionStrings.size(); ++j) {
			itemLength = static_cast<int>(lc.completionStrings[j].length());
			if (itemLength > longestCompletion) {
				longestCompletion = itemLength;
			}
		}
		longestCompletion += 2;
		int columnCount = pi.promptScreenColumns / longestCompletion;
		if (columnCount < 1) {
			columnCount = 1;
		}
		if (!onNewLine) {	// skip this if we showed "Display all %d possibilities?"
			int savePos =
					pos;	// move cursor to EOL to avoid overwriting the command line
			pos = len;
			refreshLine(pi);
			pos = savePos;
		}
		size_t pauseRow = getScreenRows() - 1;
		size_t rowCount =
				(lc.completionStrings.size() + columnCount - 1) / columnCount;
		for (size_t row = 0; row < rowCount; ++row) {
			if (row == pauseRow) {
				printf("\n--More--");
				fflush(stdout);
				c = 0;
				bool doBeep = false;
				while (c != ' ' && c != '\r' && c != '\n' && c != 'y' && c != 'Y' &&
							 c != 'n' && c != 'N' && c != 'q' && c != 'Q' &&
							 c != ctrlChar('C')) {
					if (doBeep) {
						beep();
					}
					doBeep = true;
					do {
						c = read_char();
						c = cleanupCtrl(c);
					} while (c == static_cast<char32_t>(-1));
				}
				switch (c) {
					case ' ':
					case 'y':
					case 'Y':
						printf("\r				\r");
						pauseRow += getScreenRows() - 1;
						break;
					case '\r':
					case '\n':
						printf("\r				\r");
						++pauseRow;
						break;
					case 'n':
					case 'N':
					case 'q':
					case 'Q':
						printf("\r				\r");
						stopList = true;
						break;
					case ctrlChar('C'):
						if (write(1, "^C", 2) == -1) return -1;	// Display the ^C we got
						stopList = true;
						break;
				}
			} else {
				printf("\n");
			}
			if (stopList) {
				break;
			}
			for (int column = 0; column < columnCount; ++column) {
				size_t index = (column * rowCount) + row;
				if (index < lc.completionStrings.size()) {
					itemLength = static_cast<int>(lc.completionStrings[index].length());
					fflush(stdout);
					if (write32(1, lc.completionStrings[index].get(), itemLength) == -1)
						return -1;
					if (((column + 1) * rowCount) + row < lc.completionStrings.size()) {
						for (int k = itemLength; k < longestCompletion; ++k) {
							printf(" ");
						}
					}
				}
			}
		}
		fflush(stdout);
		freeCompletions(&lc);
	}

	// display the prompt on a new line, then redisplay the input buffer
	if (!stopList || c == ctrlChar('C')) {
		if (write(1, "\n", 1) == -1) return 0;
	}
	if (!pi.write()) return 0;
#ifndef _WIN32
	// we have to generate our own newline on line wrap on Linux
	if (pi.promptIndentation == 0 && pi.promptExtraLines > 0)
		if (write(1, "\n", 1) == -1) return 0;
#endif
	pi.promptCursorRowOffset = pi.promptExtraLines;
	refreshLine(pi);
	return 0;
}

int InputBuffer::getInputLine(PromptBase& pi) {
	// The latest history entry is always our current buffer
	if (len > 0) {
		size_t bufferSize = sizeof(char32_t) * len + 1;
		unique_ptr<char[]> tempBuffer(new char[bufferSize]);
		copyString32to8(tempBuffer.get(), bufferSize, buf32);
		replxx_history_add(tempBuffer.get());
	} else {
		replxx_history_add("");
	}
	historyIndex = historyLen - 1;
	historyRecallMostRecent = false;

	// display the prompt
	if (!pi.write()) return -1;

#ifndef _WIN32
	// we have to generate our own newline on line wrap on Linux
	if (pi.promptIndentation == 0 && pi.promptExtraLines > 0)
		if (write(1, "\n", 1) == -1) return -1;
#endif

	// the cursor starts out at the end of the prompt
	pi.promptCursorRowOffset = pi.promptExtraLines;

	// kill and yank start in "other" mode
	killRing.lastAction = KillRing::actionOther;

	// when history search returns control to us, we execute its terminating
	// keystroke
	int terminatingKeystroke = -1;

	// if there is already text in the buffer, display it first
	if (len > 0) {
		refreshLine(pi);
	}

	// loop collecting characters, respond to line editing characters
	while (true) {
		int c;
		if (terminatingKeystroke == -1) {
			c = read_char();	// get a new keystroke

#ifndef _WIN32
			if (c == 0 && gotResize) {
				// caught a window resize event
				// now redraw the prompt and line
				gotResize = false;
				pi.promptScreenColumns = getScreenColumns();
				dynamicRefresh(pi, buf32, len,
											 pos);	// redraw the original prompt with current input
				continue;
			}
#endif
		} else {
			c = terminatingKeystroke;	 // use the terminating keystroke from search
			terminatingKeystroke = -1;	// clear it once we've used it
		}

		c = cleanupCtrl(c);	// convert CTRL + <char> into normal ctrl

		if (c == 0) {
			return len;
		}

		if (c == -1) {
			refreshLine(pi);
			continue;
		}

		if (c == -2) {
			if (!pi.write()) return -1;
			refreshLine(pi);
			continue;
		}

		// ctrl-I/tab, command completion, needs to be before switch statement
		if (c == ctrlChar('I') && setup.completionCallback) {
			if (pos == 0)	// SERVER-4967 -- in earlier versions, you could paste
										 // previous output
				continue;		//	back into the shell ... this output may have leading
										 //	tabs.
			// This hack (i.e. what the old code did) prevents command completion
			//	on an empty line but lets users paste text with leading tabs.

			killRing.lastAction = KillRing::actionOther;
			historyRecallMostRecent = false;

			// completeLine does the actual completion and replacement
			c = completeLine(pi);

			if (c < 0)	// return on error
				return len;

			if (c == 0)	// read next character when 0
				continue;

			// deliberate fall-through here, so we use the terminating character
		}

		switch (c) {
			case ctrlChar('A'):	// ctrl-A, move cursor to start of line
			case HOME_KEY:
				killRing.lastAction = KillRing::actionOther;
				pos = 0;
				refreshLine(pi);
				break;

			case ctrlChar('B'):	// ctrl-B, move cursor left by one character
			case LEFT_ARROW_KEY:
				killRing.lastAction = KillRing::actionOther;
				if (pos > 0) {
					--pos;
					refreshLine(pi);
				}
				break;

			case META + 'b':	// meta-B, move cursor left by one word
			case META + 'B':
			case CTRL + LEFT_ARROW_KEY:
			case META + LEFT_ARROW_KEY:	// Emacs allows Meta, bash & readline don't
				killRing.lastAction = KillRing::actionOther;
				if (pos > 0) {
					while (pos > 0 && !isCharacterAlphanumeric(buf32[pos - 1])) {
						--pos;
					}
					while (pos > 0 && isCharacterAlphanumeric(buf32[pos - 1])) {
						--pos;
					}
					refreshLine(pi);
				}
				break;

			case ctrlChar('C'):	// ctrl-C, abort this line
				killRing.lastAction = KillRing::actionOther;
				historyRecallMostRecent = false;
				errno = EAGAIN;
				--historyLen;
				free(history[historyLen]);
				// we need one last refresh with the cursor at the end of the line
				// so we don't display the next prompt over the previous input line
				pos = len;	// pass len as pos for EOL
				refreshLine(pi);
				if (write(1, "^C", 2) == -1) return -1;	// Display the ^C we got
				return -1;

			case META + 'c':	// meta-C, give word initial Cap
			case META + 'C':
				killRing.lastAction = KillRing::actionOther;
				historyRecallMostRecent = false;
				if (pos < len) {
					while (pos < len && !isCharacterAlphanumeric(buf32[pos])) {
						++pos;
					}
					if (pos < len && isCharacterAlphanumeric(buf32[pos])) {
						if (buf32[pos] >= 'a' && buf32[pos] <= 'z') {
							buf32[pos] += 'A' - 'a';
						}
						++pos;
					}
					while (pos < len && isCharacterAlphanumeric(buf32[pos])) {
						if (buf32[pos] >= 'A' && buf32[pos] <= 'Z') {
							buf32[pos] += 'a' - 'A';
						}
						++pos;
					}
					refreshLine(pi);
				}
				break;

			// ctrl-D, delete the character under the cursor
			// on an empty line, exit the shell
			case ctrlChar('D'):
				killRing.lastAction = KillRing::actionOther;
				if (len > 0 && pos < len) {
					historyRecallMostRecent = false;
					memmove(buf32 + pos, buf32 + pos + 1, sizeof(char32_t) * (len - pos));
					--len;
					refreshLine(pi);
				} else if (len == 0) {
					--historyLen;
					free(history[historyLen]);
					return -1;
				}
				break;

			case META + 'd':	// meta-D, kill word to right of cursor
			case META + 'D':
				if (pos < len) {
					historyRecallMostRecent = false;
					int endingPos = pos;
					while (endingPos < len &&
								 !isCharacterAlphanumeric(buf32[endingPos])) {
						++endingPos;
					}
					while (endingPos < len && isCharacterAlphanumeric(buf32[endingPos])) {
						++endingPos;
					}
					killRing.kill(&buf32[pos], endingPos - pos, true);
					memmove(buf32 + pos, buf32 + endingPos,
									sizeof(char32_t) * (len - endingPos + 1));
					len -= endingPos - pos;
					refreshLine(pi);
				}
				killRing.lastAction = KillRing::actionKill;
				break;

			case ctrlChar('E'):	// ctrl-E, move cursor to end of line
			case END_KEY:
				killRing.lastAction = KillRing::actionOther;
				pos = len;
				refreshLine(pi);
				break;

			case ctrlChar('F'):	// ctrl-F, move cursor right by one character
			case RIGHT_ARROW_KEY:
				killRing.lastAction = KillRing::actionOther;
				if (pos < len) {
					++pos;
					refreshLine(pi);
				}
				break;

			case META + 'f':	// meta-F, move cursor right by one word
			case META + 'F':
			case CTRL + RIGHT_ARROW_KEY:
			case META + RIGHT_ARROW_KEY:	// Emacs allows Meta, bash & readline don't
				killRing.lastAction = KillRing::actionOther;
				if (pos < len) {
					while (pos < len && !isCharacterAlphanumeric(buf32[pos])) {
						++pos;
					}
					while (pos < len && isCharacterAlphanumeric(buf32[pos])) {
						++pos;
					}
					refreshLine(pi);
				}
				break;

			case ctrlChar('H'):	// backspace/ctrl-H, delete char to left of cursor
				killRing.lastAction = KillRing::actionOther;
				if (pos > 0) {
					historyRecallMostRecent = false;
					memmove(buf32 + pos - 1, buf32 + pos,
									sizeof(char32_t) * (1 + len - pos));
					--pos;
					--len;
					refreshLine(pi);
				}
				break;

			// meta-Backspace, kill word to left of cursor
			case META + ctrlChar('H'):
				if (pos > 0) {
					historyRecallMostRecent = false;
					int startingPos = pos;
					while (pos > 0 && !isCharacterAlphanumeric(buf32[pos - 1])) {
						--pos;
					}
					while (pos > 0 && isCharacterAlphanumeric(buf32[pos - 1])) {
						--pos;
					}
					killRing.kill(&buf32[pos], startingPos - pos, false);
					memmove(buf32 + pos, buf32 + startingPos,
									sizeof(char32_t) * (len - startingPos + 1));
					len -= startingPos - pos;
					refreshLine(pi);
				}
				killRing.lastAction = KillRing::actionKill;
				break;

			case ctrlChar('J'):	// ctrl-J/linefeed/newline, accept line
			case ctrlChar('M'):	// ctrl-M/return/enter
				killRing.lastAction = KillRing::actionOther;
				// we need one last refresh with the cursor at the end of the line
				// so we don't display the next prompt over the previous input line
				pos = len;	// pass len as pos for EOL
				refreshLine(pi);
				historyPreviousIndex = historyRecallMostRecent ? historyIndex : -2;
				--historyLen;
				free(history[historyLen]);
				return len;

			case ctrlChar('K'):	// ctrl-K, kill from cursor to end of line
				killRing.kill(&buf32[pos], len - pos, true);
				buf32[pos] = '\0';
				len = pos;
				refreshLine(pi);
				killRing.lastAction = KillRing::actionKill;
				historyRecallMostRecent = false;
				break;

			case ctrlChar('L'):	// ctrl-L, clear screen and redisplay line
				clearScreen(pi);
				break;

			case META + 'l':	// meta-L, lowercase word
			case META + 'L':
				killRing.lastAction = KillRing::actionOther;
				if (pos < len) {
					historyRecallMostRecent = false;
					while (pos < len && !isCharacterAlphanumeric(buf32[pos])) {
						++pos;
					}
					while (pos < len && isCharacterAlphanumeric(buf32[pos])) {
						if (buf32[pos] >= 'A' && buf32[pos] <= 'Z') {
							buf32[pos] += 'a' - 'A';
						}
						++pos;
					}
					refreshLine(pi);
				}
				break;

			case ctrlChar('N'):	// ctrl-N, recall next line in history
			case ctrlChar('P'):	// ctrl-P, recall previous line in history
			case DOWN_ARROW_KEY:
			case UP_ARROW_KEY:
				killRing.lastAction = KillRing::actionOther;
				// if not already recalling, add the current line to the history list so
				// we don't
				// have to special case it
				if (historyIndex == historyLen - 1) {
					free(history[historyLen - 1]);
					size_t tempBufferSize = sizeof(char32_t) * len + 1;
					unique_ptr<char[]> tempBuffer(new char[tempBufferSize]);
					copyString32to8(tempBuffer.get(), tempBufferSize, buf32);
					history[historyLen - 1] = strdup8(tempBuffer.get());
				}
				if (historyLen > 1) {
					if (c == UP_ARROW_KEY) {
						c = ctrlChar('P');
					}
					if (historyPreviousIndex != -2 && c != ctrlChar('P')) {
						historyIndex =
								1 + historyPreviousIndex;	// emulate Windows down-arrow
					} else {
						historyIndex += (c == ctrlChar('P')) ? -1 : 1;
					}
					historyPreviousIndex = -2;
					if (historyIndex < 0) {
						historyIndex = 0;
						break;
					} else if (historyIndex >= historyLen) {
						historyIndex = historyLen - 1;
						break;
					}
					historyRecallMostRecent = true;
					size_t ucharCount = 0;
					copyString8to32(buf32, buflen, ucharCount, history[historyIndex]);
					len = pos = static_cast<int>(ucharCount);
					refreshLine(pi);
				}
				break;

			case ctrlChar('R'):	// ctrl-R, reverse history search
			case ctrlChar('S'):	// ctrl-S, forward history search
				terminatingKeystroke = incrementalHistorySearch(pi, c);
				break;

			case ctrlChar('T'):	// ctrl-T, transpose characters
				killRing.lastAction = KillRing::actionOther;
				if (pos > 0 && len > 1) {
					historyRecallMostRecent = false;
					size_t leftCharPos = (pos == len) ? pos - 2 : pos - 1;
					char32_t aux = buf32[leftCharPos];
					buf32[leftCharPos] = buf32[leftCharPos + 1];
					buf32[leftCharPos + 1] = aux;
					if (pos != len) ++pos;
					refreshLine(pi);
				}
				break;

			case ctrlChar(
					'U'):	// ctrl-U, kill all characters to the left of the cursor
				if (pos > 0) {
					historyRecallMostRecent = false;
					killRing.kill(&buf32[0], pos, false);
					len -= pos;
					memmove(buf32, buf32 + pos, sizeof(char32_t) * (len + 1));
					pos = 0;
					refreshLine(pi);
				}
				killRing.lastAction = KillRing::actionKill;
				break;

			case META + 'u':	// meta-U, uppercase word
			case META + 'U':
				killRing.lastAction = KillRing::actionOther;
				if (pos < len) {
					historyRecallMostRecent = false;
					while (pos < len && !isCharacterAlphanumeric(buf32[pos])) {
						++pos;
					}
					while (pos < len && isCharacterAlphanumeric(buf32[pos])) {
						if (buf32[pos] >= 'a' && buf32[pos] <= 'z') {
							buf32[pos] += 'A' - 'a';
						}
						++pos;
					}
					refreshLine(pi);
				}
				break;

			// ctrl-W, kill to whitespace (not word) to left of cursor
			case ctrlChar('W'):
				if (pos > 0) {
					historyRecallMostRecent = false;
					int startingPos = pos;
					while (pos > 0 && buf32[pos - 1] == ' ') {
						--pos;
					}
					while (pos > 0 && buf32[pos - 1] != ' ') {
						--pos;
					}
					killRing.kill(&buf32[pos], startingPos - pos, false);
					memmove(buf32 + pos, buf32 + startingPos,
									sizeof(char32_t) * (len - startingPos + 1));
					len -= startingPos - pos;
					refreshLine(pi);
				}
				killRing.lastAction = KillRing::actionKill;
				break;

			case ctrlChar('Y'):	// ctrl-Y, yank killed text
				historyRecallMostRecent = false;
				{
					Utf32String* restoredText = killRing.yank();
					if (restoredText) {
						bool truncated = false;
						size_t ucharCount = restoredText->length();
						if (ucharCount > static_cast<size_t>(buflen - len)) {
							ucharCount = buflen - len;
							truncated = true;
						}
						memmove(buf32 + pos + ucharCount, buf32 + pos,
										sizeof(char32_t) * (len - pos + 1));
						memmove(buf32 + pos, restoredText->get(),
										sizeof(char32_t) * ucharCount);
						pos += static_cast<int>(ucharCount);
						len += static_cast<int>(ucharCount);
						refreshLine(pi);
						killRing.lastAction = KillRing::actionYank;
						killRing.lastYankSize = ucharCount;
						if (truncated) {
							beep();
						}
					} else {
						beep();
					}
				}
				break;

			case META + 'y':	// meta-Y, "yank-pop", rotate popped text
			case META + 'Y':
				if (killRing.lastAction == KillRing::actionYank) {
					historyRecallMostRecent = false;
					Utf32String* restoredText = killRing.yankPop();
					if (restoredText) {
						bool truncated = false;
						size_t ucharCount = restoredText->length();
						if (ucharCount >
								static_cast<size_t>(killRing.lastYankSize + buflen - len)) {
							ucharCount = killRing.lastYankSize + buflen - len;
							truncated = true;
						}
						if (ucharCount > killRing.lastYankSize) {
							memmove(buf32 + pos + ucharCount - killRing.lastYankSize,
											buf32 + pos, sizeof(char32_t) * (len - pos + 1));
							memmove(buf32 + pos - killRing.lastYankSize, restoredText->get(),
											sizeof(char32_t) * ucharCount);
						} else {
							memmove(buf32 + pos - killRing.lastYankSize, restoredText->get(),
											sizeof(char32_t) * ucharCount);
							memmove(buf32 + pos + ucharCount - killRing.lastYankSize,
											buf32 + pos, sizeof(char32_t) * (len - pos + 1));
						}
						pos += static_cast<int>(ucharCount - killRing.lastYankSize);
						len += static_cast<int>(ucharCount - killRing.lastYankSize);
						killRing.lastYankSize = ucharCount;
						refreshLine(pi);
						if (truncated) {
							beep();
						}
						break;
					}
				}
				beep();
				break;

#ifndef _WIN32
			case ctrlChar('Z'):	// ctrl-Z, job control
				disableRawMode();	// Returning to Linux (whatever) shell, leave raw
													 // mode
				raise(SIGSTOP);		// Break out in mid-line
				enableRawMode();	 // Back from Linux shell, re-enter raw mode
				if (!pi.write()) break;	// Redraw prompt
				refreshLine(pi);				 // Refresh the line
				break;
#endif

			// DEL, delete the character under the cursor
			case 127:
			case DELETE_KEY:
				killRing.lastAction = KillRing::actionOther;
				if (len > 0 && pos < len) {
					historyRecallMostRecent = false;
					memmove(buf32 + pos, buf32 + pos + 1, sizeof(char32_t) * (len - pos));
					--len;
					refreshLine(pi);
				}
				break;

			case META + '<':		 // meta-<, beginning of history
			case PAGE_UP_KEY:		// Page Up, beginning of history
			case META + '>':		 // meta->, end of history
			case PAGE_DOWN_KEY:	// Page Down, end of history
				killRing.lastAction = KillRing::actionOther;
				// if not already recalling, add the current line to the history list so
				// we don't
				// have to special case it
				if (historyIndex == historyLen - 1) {
					free(history[historyLen - 1]);
					size_t tempBufferSize = sizeof(char32_t) * len + 1;
					unique_ptr<char[]> tempBuffer(new char[tempBufferSize]);
					copyString32to8(tempBuffer.get(), tempBufferSize, buf32);
					history[historyLen - 1] = strdup8(tempBuffer.get());
				}
				if (historyLen > 1) {
					historyIndex =
							(c == META + '<' || c == PAGE_UP_KEY) ? 0 : historyLen - 1;
					historyPreviousIndex = -2;
					historyRecallMostRecent = true;
					size_t ucharCount = 0;
					copyString8to32(buf32, buflen, ucharCount, history[historyIndex]);
					len = pos = static_cast<int>(ucharCount);
					refreshLine(pi);
				}
				break;

			// not one of our special characters, maybe insert it in the buffer
			default:
				killRing.lastAction = KillRing::actionOther;
				historyRecallMostRecent = false;
				if (c & (META | CTRL)) {	// beep on unknown Ctrl and/or Meta keys
					beep();
					break;
				}
				if (len < buflen) {
					if (isControlChar(c)) {	// don't insert control characters
						beep();
						break;
					}
					if (len == pos) {	// at end of buffer
						buf32[pos] = c;
						++pos;
						++len;
						buf32[len] = '\0';
						int inputLen = calculateColumnPosition(buf32, len);
						if (pi.promptIndentation + inputLen < pi.promptScreenColumns) {
							if (inputLen > pi.promptPreviousInputLen)
								pi.promptPreviousInputLen = inputLen;
							/* Avoid a full update of the line in the
							 * trivial case. */
							if (write32(1, reinterpret_cast<char32_t*>(&c), 1) == -1)
								return -1;
						} else {
							refreshLine(pi);
						}
					} else {	// not at end of buffer, have to move characters to our
										// right
						memmove(buf32 + pos + 1, buf32 + pos,
										sizeof(char32_t) * (len - pos));
						buf32[pos] = c;
						++len;
						++pos;
						buf32[len] = '\0';
						refreshLine(pi);
					}
				} else {
					beep();	// buffer is full, beep on new characters
				}
				break;
		}
	}
	return len;
}

/**
 * Incremental history search -- take over the prompt and keyboard as the user
 * types a search
 * string, deletes characters from it, changes direction, and either accepts the
 * found line (for
 * execution orediting) or cancels.
 * @param pi				PromptBase struct holding information about the (old,
 * static) prompt and our
 *									screen position
 * @param startChar the character that began the search, used to set the initial
 * direction
 */
int InputBuffer::incrementalHistorySearch(PromptBase& pi, int startChar) {
	size_t bufferSize;
	size_t ucharCount = 0;

	// if not already recalling, add the current line to the history list so we
	// don't have to
	// special case it
	if (historyIndex == historyLen - 1) {
		free(history[historyLen - 1]);
		bufferSize = sizeof(char32_t) * len + 1;
		unique_ptr<char[]> tempBuffer(new char[bufferSize]);
		copyString32to8(tempBuffer.get(), bufferSize, buf32);
		history[historyLen - 1] = strdup8(tempBuffer.get());
	}
	int historyLineLength = len;
	int historyLinePosition = pos;
	char32_t emptyBuffer[1];
	char emptyWidths[1];
	InputBuffer empty(emptyBuffer, emptyWidths, 1);
	empty.refreshLine(pi);	// erase the old input first
	DynamicPrompt dp(pi, (startChar == ctrlChar('R')) ? -1 : 1);

	dp.promptPreviousLen = pi.promptPreviousLen;
	dp.promptPreviousInputLen = pi.promptPreviousInputLen;
	dynamicRefresh(dp, buf32, historyLineLength,
								 historyLinePosition);	// draw user's text with our prompt

	// loop until we get an exit character
	int c;
	bool keepLooping = true;
	bool useSearchedLine = true;
	bool searchAgain = false;
	char32_t* activeHistoryLine = 0;
	while (keepLooping) {
		c = read_char();
		c = cleanupCtrl(c);	// convert CTRL + <char> into normal ctrl

		switch (c) {
			// these characters keep the selected text but do not execute it
			case ctrlChar('A'):	// ctrl-A, move cursor to start of line
			case HOME_KEY:
			case ctrlChar('B'):	// ctrl-B, move cursor left by one character
			case LEFT_ARROW_KEY:
			case META + 'b':	// meta-B, move cursor left by one word
			case META + 'B':
			case CTRL + LEFT_ARROW_KEY:
			case META + LEFT_ARROW_KEY:	// Emacs allows Meta, bash & readline don't
			case ctrlChar('D'):
			case META + 'd':	// meta-D, kill word to right of cursor
			case META + 'D':
			case ctrlChar('E'):	// ctrl-E, move cursor to end of line
			case END_KEY:
			case ctrlChar('F'):	// ctrl-F, move cursor right by one character
			case RIGHT_ARROW_KEY:
			case META + 'f':	// meta-F, move cursor right by one word
			case META + 'F':
			case CTRL + RIGHT_ARROW_KEY:
			case META + RIGHT_ARROW_KEY:	// Emacs allows Meta, bash & readline don't
			case META + ctrlChar('H'):
			case ctrlChar('J'):
			case ctrlChar('K'):	// ctrl-K, kill from cursor to end of line
			case ctrlChar('M'):
			case ctrlChar('N'):	// ctrl-N, recall next line in history
			case ctrlChar('P'):	// ctrl-P, recall previous line in history
			case DOWN_ARROW_KEY:
			case UP_ARROW_KEY:
			case ctrlChar('T'):	// ctrl-T, transpose characters
			case ctrlChar(
					'U'):	// ctrl-U, kill all characters to the left of the cursor
			case ctrlChar('W'):
			case META + 'y':	// meta-Y, "yank-pop", rotate popped text
			case META + 'Y':
			case 127:
			case DELETE_KEY:
			case META + '<':	// start of history
			case PAGE_UP_KEY:
			case META + '>':	// end of history
			case PAGE_DOWN_KEY:
				keepLooping = false;
				break;

			// these characters revert the input line to its previous state
			case ctrlChar('C'):	// ctrl-C, abort this line
			case ctrlChar('G'):
			case ctrlChar('L'):	// ctrl-L, clear screen and redisplay line
				keepLooping = false;
				useSearchedLine = false;
				if (c != ctrlChar('L')) {
					c = -1;	// ctrl-C and ctrl-G just abort the search and do nothing
									 // else
				}
				break;

			// these characters stay in search mode and update the display
			case ctrlChar('S'):
			case ctrlChar('R'):
				if (dp.searchTextLen ==
						0) {	// if no current search text, recall previous text
					if (previousSearchText.length()) {
						dp.updateSearchText(previousSearchText.get());
					}
				}
				if ((dp.direction == 1 && c == ctrlChar('R')) ||
						(dp.direction == -1 && c == ctrlChar('S'))) {
					dp.direction = 0 - dp.direction;	// reverse direction
					dp.updateSearchPrompt();					// change the prompt
				} else {
					searchAgain = true;	// same direction, search again
				}
				break;

// job control is its own thing
#ifndef _WIN32
			case ctrlChar('Z'):	// ctrl-Z, job control
				disableRawMode();	// Returning to Linux (whatever) shell, leave raw
													 // mode
				raise(SIGSTOP);		// Break out in mid-line
				enableRawMode();	 // Back from Linux shell, re-enter raw mode
				{
					bufferSize = historyLineLength + 1;
					unique_ptr<char32_t[]> tempUnicode(new char32_t[bufferSize]);
					copyString8to32(tempUnicode.get(), bufferSize, ucharCount,
													history[historyIndex]);
					dynamicRefresh(dp, tempUnicode.get(), historyLineLength,
												 historyLinePosition);
				}
				continue;
				break;
#endif

			// these characters update the search string, and hence the selected input
			// line
			case ctrlChar('H'):	// backspace/ctrl-H, delete char to left of cursor
				if (dp.searchTextLen > 0) {
					unique_ptr<char32_t[]> tempUnicode(new char32_t[dp.searchTextLen]);
					--dp.searchTextLen;
					dp.searchText[dp.searchTextLen] = 0;
					copyString32(tempUnicode.get(), dp.searchText.get(),
											 dp.searchTextLen);
					dp.updateSearchText(tempUnicode.get());
				} else {
					beep();
				}
				break;

			case ctrlChar('Y'):	// ctrl-Y, yank killed text
				break;

			default:
				if (!isControlChar(c) && c <= 0x0010FFFF) {	// not an action character
					unique_ptr<char32_t[]> tempUnicode(
							new char32_t[dp.searchTextLen + 2]);
					copyString32(tempUnicode.get(), dp.searchText.get(),
											 dp.searchTextLen);
					tempUnicode[dp.searchTextLen] = c;
					tempUnicode[dp.searchTextLen + 1] = 0;
					dp.updateSearchText(tempUnicode.get());
				} else {
					beep();
				}
		}	// switch

		// if we are staying in search mode, search now
		if (keepLooping) {
			bufferSize = historyLineLength + 1;
			activeHistoryLine = new char32_t[bufferSize];
			copyString8to32(activeHistoryLine, bufferSize, ucharCount,
											history[historyIndex]);
			if (dp.searchTextLen > 0) {
				bool found = false;
				int historySearchIndex = historyIndex;
				int lineLength = static_cast<int>(ucharCount);
				int lineSearchPos = historyLinePosition;
				if (searchAgain) {
					lineSearchPos += dp.direction;
				}
				searchAgain = false;
				while (true) {
					while ((dp.direction > 0) ? (lineSearchPos < lineLength)
																		: (lineSearchPos >= 0)) {
						if (strncmp32(dp.searchText.get(),
													&activeHistoryLine[lineSearchPos],
													dp.searchTextLen) == 0) {
							found = true;
							break;
						}
						lineSearchPos += dp.direction;
					}
					if (found) {
						historyIndex = historySearchIndex;
						historyLineLength = lineLength;
						historyLinePosition = lineSearchPos;
						break;
					} else if ((dp.direction > 0) ? (historySearchIndex < historyLen - 1)
																				: (historySearchIndex > 0)) {
						historySearchIndex += dp.direction;
						bufferSize = strlen8(history[historySearchIndex]) + 1;
						delete[] activeHistoryLine;
						activeHistoryLine = new char32_t[bufferSize];
						copyString8to32(activeHistoryLine, bufferSize, ucharCount,
														history[historySearchIndex]);
						lineLength = static_cast<int>(ucharCount);
						lineSearchPos =
								(dp.direction > 0) ? 0 : (lineLength - dp.searchTextLen);
					} else {
						beep();
						break;
					}
				};	// while
			}
			if (activeHistoryLine) {
				delete[] activeHistoryLine;
			}
			bufferSize = historyLineLength + 1;
			activeHistoryLine = new char32_t[bufferSize];
			copyString8to32(activeHistoryLine, bufferSize, ucharCount,
											history[historyIndex]);
			dynamicRefresh(dp, activeHistoryLine, historyLineLength,
										 historyLinePosition);	// draw user's text with our prompt
		}
	}	// while

	// leaving history search, restore previous prompt, maybe make searched line
	// current
	PromptBase pb;
	pb.promptChars = pi.promptIndentation;
	pb.promptBytes = pi.promptBytes;
	Utf32String tempUnicode(pb.promptBytes + 1);

	copyString32(tempUnicode.get(), &pi.promptText[pi.promptLastLinePosition],
							 pb.promptBytes - pi.promptLastLinePosition);
	tempUnicode.initFromBuffer();
	pb.promptText = tempUnicode;
	pb.promptExtraLines = 0;
	pb.promptIndentation = pi.promptIndentation;
	pb.promptLastLinePosition = 0;
	pb.promptPreviousInputLen = historyLineLength;
	pb.promptCursorRowOffset = dp.promptCursorRowOffset;
	pb.promptScreenColumns = pi.promptScreenColumns;
	pb.promptPreviousLen = dp.promptChars;
	if (useSearchedLine && activeHistoryLine) {
		historyRecallMostRecent = true;
		copyString32(buf32, activeHistoryLine, buflen + 1);
		len = historyLineLength;
		pos = historyLinePosition;
	}
	if (activeHistoryLine) {
		delete[] activeHistoryLine;
	}
	dynamicRefresh(pb, buf32, len,
								 pos);	// redraw the original prompt with current input
	pi.promptPreviousInputLen = len;
	pi.promptCursorRowOffset = pi.promptExtraLines + pb.promptCursorRowOffset;
	previousSearchText =
			dp.searchText;	// save search text for possible reuse on ctrl-R ctrl-R
	return c;					 // pass a character or -1 back to main loop
}

}
