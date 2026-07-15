#include "text_editor.h"
#include <cstring>
#include <algorithm>

// --- Text buffer ---
static char textBuffer[TEXT_BUFFER_SIZE];
static size_t textLength = 0;
static int cursorPosition = 0;

// --- File metadata ---
static char currentFile[MAX_FILENAME_LEN] = "";
static char currentTitle[MAX_TITLE_LEN] = "Untitled";
static bool unsavedChanges = false;

// --- Line management ---
// Provenance: incremental word-count and line-wrap ported from Mark I
// (xteink-writer/src/text_editor.cpp, commit 786c27a).
static int linePositions[MAX_LINES];  // Index into textBuffer for start of each line
static int lineScratch[MAX_LINES];    // Scratch copy of the pre-edit line-start suffix, used by the incremental resync below to diff against post-edit offsets.
static int lineCount = 0;
static int cursorLine = 0;
static int cursorCol = 0;
static int viewportStartLine = 0;
static int charsPerLine = 40;
static int storedVisibleLines = 20;  // Updated by renderer each frame
static bool lineBreaksDirty = true;  // Only recompute line breaks when buffer/charsPerLine changes
// Line index to resync from after a single-char insert/delete, or -1 to force a full rebuild
// (whole-buffer load, charsPerLine change). Valid only when lineBreaksDirty is true.
static int lineBreaksDirtyFrom = -1;
static int pendingEditDelta = 0;  // +1 insert / -1 delete; valid only when lineBreaksDirtyFrom >= 0

// Word count is maintained incrementally per edit (see applyInsertWordCountDelta /
// applyDeleteWordCountDelta) instead of rescanning the whole buffer every keystroke.
static int cachedWordCount = 0;

// Forward declaration
static void ensureCursorVisible(int visibleLines);

// Provenance: Mark I (xteink-writer/src/text_editor.cpp, commit 786c27a)
static inline bool isWordBoundaryChar(char c) {
  return c == ' ' || c == '\n' || c == '\t' || c == '\r';
}

static void recomputeWordCountFull() {
  cachedWordCount = 0;
  bool inWord = false;
  for (size_t i = 0; i < textLength; i++) {
    if (isWordBoundaryChar(textBuffer[i])) {
      inWord = false;
    } else if (!inWord) {
      cachedWordCount++;
      inWord = true;
    }
  }
}

static inline bool isBoundaryOrMissing(int idx) {
  if (idx < 0 || idx >= (int)textLength) return true;
  return isWordBoundaryChar(textBuffer[idx]);
}

// Call BEFORE inserting: p is the pre-edit position character c will be inserted at.
static void applyInsertWordCountDelta(int p, char c) {
  const bool cIsWord = !isWordBoundaryChar(c);
  const bool lBoundary = isBoundaryOrMissing(p - 1);
  const bool rBoundary = isBoundaryOrMissing(p);
  if (cIsWord && lBoundary && rBoundary) {
    cachedWordCount++;  // new isolated word created between two separators
  } else if (!cIsWord && !lBoundary && !rBoundary) {
    cachedWordCount++;  // splits one run into two
  }
}

// Call BEFORE removing: p is the pre-edit position of the character being deleted.
static void applyDeleteWordCountDelta(int p) {
  const bool cIsWord = !isWordBoundaryChar(textBuffer[p]);
  const bool lBoundary = isBoundaryOrMissing(p - 1);
  const bool rBoundary = isBoundaryOrMissing(p + 1);
  if (cIsWord && lBoundary && rBoundary) {
    cachedWordCount--;  // removed the sole char of an isolated word
  } else if (!cIsWord && !lBoundary && !rBoundary) {
    cachedWordCount--;  // merges two runs into one
  }
}

// Walk back over consecutive soft (non-newline) breaks to find the true safe resync start.
static int resolveResyncStartLine(int candidateLine) {
  while (candidateLine > 0 && textBuffer[linePositions[candidateLine] - 1] != '\n') {
    candidateLine--;
  }
  return candidateLine;
}

// Recalculate line breaks (word wrap) and cursor position.
// The O(textLength) line break loop only runs on a whole-buffer load or charsPerLine change
// (lineBreaksDirtyFrom == -1); a single-char insert/delete instead resyncs forward from the
// edited line only, stopping as soon as a post-edit line-start offset lines up with a
// pre-edit one (everything after that point is guaranteed byte-identical to the old wrap).
// Cursor line/col is always recomputed (cheap O(cursorLine) with early exit).
void editorRecalculateLines() {
  if (lineBreaksDirty) {
    if (lineBreaksDirtyFrom < 0) {
      // Full rebuild (whole-buffer load, charsPerLine change)
      lineCount = 0;
      linePositions[0] = 0;
      lineCount = 1;

      int col = 0;
      int lastSpace = -1;

      for (int i = 0; i < (int)textLength && lineCount < MAX_LINES; i++) {
        if (textBuffer[i] == '\n') {
          if (lineCount < MAX_LINES) {
            linePositions[lineCount] = i + 1;
            lineCount++;
          }
          col = 0;
          lastSpace = -1;
          continue;
        }

        if (textBuffer[i] == ' ') {
          lastSpace = i;
        }

        col++;
        if (col >= charsPerLine) {
          int breakPos;
          if (lastSpace > linePositions[lineCount - 1]) {
            breakPos = lastSpace + 1;
          } else {
            breakPos = i + 1;
          }

          if (lineCount < MAX_LINES) {
            linePositions[lineCount] = breakPos;
            lineCount++;
          }
          col = i + 1 - breakPos;
          lastSpace = -1;
        }
      }
      recomputeWordCountFull();
    } else {
      // Incremental resync from edited line
      const int startLine = lineBreaksDirtyFrom;

      // Snapshot the pre-edit suffix before overwriting linePositions[] in place.
      const int oldSuffixLen = lineCount - startLine;
      for (int k = 0; k < oldSuffixLen; k++) {
        lineScratch[k] = linePositions[startLine + k];
      }

      lineCount = startLine + 1;  // linePositions[startLine] itself is unaffected by the edit
      int oldIdx = 1;             // lineScratch[0] == linePositions[startLine], already matched

      int col = 0;
      int lastSpace = -1;

      for (int i = linePositions[startLine]; i < (int)textLength && lineCount < MAX_LINES; i++) {
        if (textBuffer[i] == '\n') {
          if (lineCount < MAX_LINES) {
            linePositions[lineCount] = i + 1;
            lineCount++;
          }
          col = 0;
          lastSpace = -1;
          // Check convergence: if this offset matches the old one, everything after is identical
          if (oldIdx < oldSuffixLen && linePositions[lineCount - 1] == lineScratch[oldIdx]) {
            // Copy remaining old offsets — the rest of the wrap is byte-identical
            for (int r = oldIdx + 1; r < oldSuffixLen; r++) {
              if (lineCount < MAX_LINES) {
                linePositions[lineCount] = lineScratch[r];
                lineCount++;
              }
            }
            goto resync_done;
          }
          oldIdx++;
          continue;
        }

        if (textBuffer[i] == ' ') {
          lastSpace = i;
        }

        col++;
        if (col >= charsPerLine) {
          int breakPos;
          if (lastSpace > linePositions[lineCount - 1]) {
            breakPos = lastSpace + 1;
          } else {
            breakPos = i + 1;
          }

          if (lineCount < MAX_LINES) {
            linePositions[lineCount] = breakPos;
            lineCount++;
          }
          col = i + 1 - breakPos;
          lastSpace = -1;

          if (oldIdx < oldSuffixLen && linePositions[lineCount - 1] == lineScratch[oldIdx]) {
            for (int r = oldIdx + 1; r < oldSuffixLen; r++) {
              if (lineCount < MAX_LINES) {
                linePositions[lineCount] = lineScratch[r];
                lineCount++;
              }
            }
            goto resync_done;
          }
          oldIdx++;
        }
      }

    resync_done:
      ;
    }
    lineBreaksDirty = false;
    lineBreaksDirtyFrom = -1;
    pendingEditDelta = 0;
  }

  // Compute cursor line and column (always — cheap O(cursorLine) with early exit)
  cursorLine = 0;
  for (int i = 1; i < lineCount; i++) {
    if (cursorPosition >= linePositions[i]) {
      cursorLine = i;
    } else {
      break;
    }
  }
  cursorCol = cursorPosition - linePositions[cursorLine];
}

// Ensure cursor is visible by adjusting viewport
static void ensureCursorVisible(int visibleLines) {
  if (visibleLines <= 0) visibleLines = 20; // fallback

  if (cursorLine < viewportStartLine) {
    viewportStartLine = cursorLine;
  } else if (cursorLine >= viewportStartLine + visibleLines) {
    viewportStartLine = cursorLine - visibleLines + 1;
  }

  if (viewportStartLine < 0) viewportStartLine = 0;
  if (viewportStartLine >= lineCount) viewportStartLine = std::max(0, lineCount - 1);
}

void editorInit() {
  memset(textBuffer, 0, TEXT_BUFFER_SIZE);
  textLength = 0;
  cursorPosition = 0;
  currentFile[0] = '\0';
  strncpy(currentTitle, "Untitled", MAX_TITLE_LEN - 1);
  unsavedChanges = false;
  viewportStartLine = 0;
  cachedWordCount = 0;
  lineBreaksDirty = true;
  lineBreaksDirtyFrom = -1;
  editorRecalculateLines();
}

void editorClear() {
  memset(textBuffer, 0, TEXT_BUFFER_SIZE);
  textLength = 0;
  cursorPosition = 0;
  unsavedChanges = false;
  viewportStartLine = 0;
  cachedWordCount = 0;
  lineBreaksDirty = true;
  lineBreaksDirtyFrom = -1;
  editorRecalculateLines();
}

void editorLoadBuffer(size_t length) {
  textLength = length;
  textBuffer[textLength] = '\0';
  cursorPosition = (int)textLength;  // Start at end
  viewportStartLine = 0;
  lineBreaksDirty = true;
  lineBreaksDirtyFrom = -1;  // Full rebuild
  editorRecalculateLines();
  // Scroll to show cursor
  ensureCursorVisible(storedVisibleLines);
}

char* editorGetBuffer() { return textBuffer; }
size_t editorGetLength() { return textLength; }
int editorGetCursorPosition() { return cursorPosition; }

int editorGetWordCount() {
  // Incremental word count — maintained per edit, not rescanned every call.
  // Provenance: Mark I (xteink-writer/src/text_editor.cpp, commit 786c27a).
  return cachedWordCount;
}

void editorInsertChar(char c) {
  if (textLength >= TEXT_BUFFER_SIZE - 1) return;

  // Incremental word count delta (before the insert changes the buffer)
  applyInsertWordCountDelta(cursorPosition, c);

  // Shift text right
  for (int i = (int)textLength; i > cursorPosition; i--) {
    textBuffer[i] = textBuffer[i - 1];
  }
  textBuffer[cursorPosition] = c;
  cursorPosition++;
  textLength++;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;

  // Incremental line resync from the edited line
  lineBreaksDirty = true;
  lineBreaksDirtyFrom = cursorLine;  // resync from current line
  lineBreaksDirtyFrom = resolveResyncStartLine(lineBreaksDirtyFrom);
  pendingEditDelta = 1;

  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorDeleteChar() {
  if (cursorPosition <= 0 || textLength == 0) return;

  // Incremental word count delta (before the delete changes the buffer)
  applyDeleteWordCountDelta(cursorPosition - 1);

  for (int i = cursorPosition - 1; i < (int)textLength - 1; i++) {
    textBuffer[i] = textBuffer[i + 1];
  }
  cursorPosition--;
  textLength--;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;

  // Incremental line resync from the edited line
  lineBreaksDirty = true;
  lineBreaksDirtyFrom = cursorLine;
  lineBreaksDirtyFrom = resolveResyncStartLine(lineBreaksDirtyFrom);
  pendingEditDelta = -1;

  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorDeleteForward() {
  if (cursorPosition >= (int)textLength) return;

  // Incremental word count delta
  applyDeleteWordCountDelta(cursorPosition);

  for (int i = cursorPosition; i < (int)textLength - 1; i++) {
    textBuffer[i] = textBuffer[i + 1];
  }
  textLength--;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;

  // Incremental line resync
  lineBreaksDirty = true;
  lineBreaksDirtyFrom = cursorLine;
  lineBreaksDirtyFrom = resolveResyncStartLine(lineBreaksDirtyFrom);
  pendingEditDelta = -1;

  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorLeft() {
  if (cursorPosition > 0) {
    cursorPosition--;
    editorRecalculateLines();
    ensureCursorVisible(storedVisibleLines);
  }
}

void editorMoveCursorRight() {
  if (cursorPosition < (int)textLength) {
    cursorPosition++;
    editorRecalculateLines();
    ensureCursorVisible(storedVisibleLines);
  }
}

void editorMoveCursorUp() {
  // cursorLine/cursorCol are already valid from the previous operation
  if (cursorLine <= 0) return;

  int targetLine = cursorLine - 1;
  int lineStart = linePositions[targetLine];
  int lineEnd = (targetLine + 1 < lineCount) ? linePositions[targetLine + 1] : (int)textLength;
  int lineLen = lineEnd - lineStart;
  // Don't count trailing newline
  if (lineLen > 0 && textBuffer[lineStart + lineLen - 1] == '\n') lineLen--;

  cursorPosition = lineStart + std::min(cursorCol, lineLen);
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorDown() {
  if (cursorLine >= lineCount - 1) return;

  int targetLine = cursorLine + 1;
  int lineStart = linePositions[targetLine];
  int lineEnd = (targetLine + 1 < lineCount) ? linePositions[targetLine + 1] : (int)textLength;
  int lineLen = lineEnd - lineStart;
  if (lineLen > 0 && textBuffer[lineStart + lineLen - 1] == '\n') lineLen--;

  cursorPosition = lineStart + std::min(cursorCol, lineLen);
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorHome() {
  cursorPosition = linePositions[cursorLine];
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorEnd() {
  int lineEnd;
  if (cursorLine + 1 < lineCount) {
    lineEnd = linePositions[cursorLine + 1];
    // Step back over newline if present
    if (lineEnd > 0 && textBuffer[lineEnd - 1] == '\n') lineEnd--;
  } else {
    lineEnd = (int)textLength;
  }
  cursorPosition = lineEnd;
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorSetCharsPerLine(int cpl) {
  if (cpl != charsPerLine) {
    charsPerLine = cpl;
    lineBreaksDirty = true;
  }
  editorRecalculateLines();
}

void editorSetVisibleLines(int n) {
  if (n > 0) storedVisibleLines = n;
}

int editorGetStoredVisibleLines() {
  return storedVisibleLines;
}

int editorGetVisibleLines(int lineHeight, int textAreaHeight) {
  if (lineHeight <= 0) return 20;
  return textAreaHeight / lineHeight;
}

int editorGetViewportStart() { return viewportStartLine; }
int editorGetCursorLine() { return cursorLine; }
int editorGetCursorCol() { return cursorCol; }
int editorGetLineCount() { return lineCount; }

int editorGetLinePosition(int lineIndex) {
  if (lineIndex < 0 || lineIndex >= lineCount) return 0;
  return linePositions[lineIndex];
}

void editorSetCurrentFile(const char* filename) {
  strncpy(currentFile, filename, MAX_FILENAME_LEN - 1);
  currentFile[MAX_FILENAME_LEN - 1] = '\0';
}

void editorSetCurrentTitle(const char* title) {
  strncpy(currentTitle, title, MAX_TITLE_LEN - 1);
  currentTitle[MAX_TITLE_LEN - 1] = '\0';
}

const char* editorGetCurrentFile() { return currentFile; }
const char* editorGetCurrentTitle() { return currentTitle; }
bool editorHasUnsavedChanges() { return unsavedChanges; }
void editorSetUnsavedChanges(bool v) { unsavedChanges = v; }
