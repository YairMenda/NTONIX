#!/bin/bash
# Ralph Wiggum - Robust Autonomous Coding Loop
# Usage: ./ralph.sh [max_iterations]

set -e

# --- Configuration & Paths ---
MAX=${1:-10}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRD_FILE="$SCRIPT_DIR/PRD.md"
PROGRESS_FILE="$SCRIPT_DIR/progress.txt"
ARCHIVE_DIR="$SCRIPT_DIR/archive"
LAST_BRANCH_FILE="$SCRIPT_DIR/.last-branch"

# --- State Management: Archiving ---
# Archives previous runs if the git branch has changed to ensure a clean state.
if [ -f "$LAST_BRANCH_FILE" ] && command -v git >/dev/null; then
  CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "main")
  LAST_BRANCH=$(cat "$LAST_BRANCH_FILE" 2>/dev/null || echo "")
  
  if [ -n "$LAST_BRANCH" ] && [ "$CURRENT_BRANCH" != "$LAST_BRANCH" ]; then
    DATE=$(date +%Y-%m-%d)
    ARCHIVE_FOLDER="$ARCHIVE_DIR/$DATE-$LAST_BRANCH"
    
    echo "Archiving previous run: $LAST_BRANCH"
    mkdir -p "$ARCHIVE_FOLDER"
    [ -f "$PRD_FILE" ] && cp "$PRD_FILE" "$ARCHIVE_FOLDER/"
    [ -f "$PROGRESS_FILE" ] && cp "$PROGRESS_FILE" "$ARCHIVE_FOLDER/"
    
    # Reset progress for the new branch
    echo "# Ralph Progress Log" > "$PROGRESS_FILE"
    echo "Started: $(date)" >> "$PROGRESS_FILE"
    echo "---" >> "$PROGRESS_FILE"
  fi
  echo "$CURRENT_BRANCH" > "$LAST_BRANCH_FILE"
fi

# Ensure progress file exists before starting
if [ ! -f "$PROGRESS_FILE" ]; then
  echo "# Ralph Progress Log" > "$PROGRESS_FILE"
  echo "---" >> "$PROGRESS_FILE"
fi

echo "Starting Ralph - Max $MAX iterations"
echo "Target: $PRD_FILE"
echo ""

# --- Main Iteration Loop ---
for ((i=1; i<=$MAX; i++)); do
  echo "================================================================================"
  echo "  Ralph Iteration $i of $MAX"
  echo "================================================================================"

  # Execute the agent and capture output using tee to show progress in real-time
  result=$(claude --dangerously-skip-permissions --output-format text -p "@PRD.md @progress.txt
You are Ralph, an autonomous coding agent. Do exactly ONE task per iteration.

## Steps
1. Read PRD.md and find the first task that is NOT complete (marked [ ]).
2. Read progress.txt - check the 'Learnings' section first for patterns from previous iterations.
3. Implement that ONE task only.
4. Run tests/typecheck to verify it works.

## Critical: Only Complete If Tests Pass
- If tests PASS:
  - Update PRD.md to mark the task complete (change [ ] to [x]).
  - Commit your changes with message: feat: [task description].
  - Append implementation details to progress.txt using the format below.

- If tests FAIL:
  - Do NOT mark the task complete.
  - Do NOT commit broken code.
  - Append what went wrong to progress.txt so the next iteration can learn.

## Progress Notes Format
Append to progress.txt:
---
## Iteration [$i] - [Task Name]
- What was implemented
- Files changed
- Learnings for future iterations:
  - Patterns discovered
  - Gotchas encountered
  - Useful context
---

## Update AGENTS.md (If Applicable)
If you discover a reusable pattern that future work should know about:
- Check if AGENTS.md exists in the project root.
- Add patterns like: \"This codebase uses X for Y\" or \"Always do Z when changing W\".
- Only add genuinely reusable knowledge, not task-specific details.

## End Condition
After completing your task, check PRD.md:
- If ALL tasks are [x], output exactly: <promise>COMPLETE</promise>.
- If tasks remain [ ], just end your response (next iteration will continue).
" 2>&1 | tee /dev/stderr)

  # Check for completion signal to break the loop
  if echo "$result" | grep -q "<promise>COMPLETE</promise>"; then
    echo ""
    echo "âœ… Ralph completed all tasks in PRD.md!"
    exit 0
  fi

  echo ""
  echo "Iteration $i complete. Continuing to next iteration..."
  sleep 2
done

echo ""
echo "âŒ Ralph reached max iterations ($MAX) without completing all tasks."
echo "Check $PROGRESS_FILE for the last known state."
exit 1