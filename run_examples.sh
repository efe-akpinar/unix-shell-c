#!/bin/sh
# Replays every session in examples/sessions/ through ./shell and compares the
# output with the recorded transcript (prompts and echoed commands are ignored).
# Leading whitespace is ignored because BSD and GNU `wc` pad counts differently.
# Each session runs in a temporary copy of examples/, so no files are left behind.

ROOT=$(cd "$(dirname "$0")" && pwd)
SHELL_BIN="$ROOT/shell"
PROMPT='shell$ '

[ -x "$SHELL_BIN" ] || { echo "Build first: make"; exit 1; }

pass=0
fail=0
for session in "$ROOT"/examples/sessions/*.txt; do
    name=$(basename "$session" .txt)
    work=$(mktemp -d)
    cp "$ROOT"/examples/*.txt "$work"/

    # Transcript lines starting with the prompt are commands; the rest is output.
    awk -v p="$PROMPT" 'index($0, p) == 1 { print substr($0, length(p) + 1) }' "$session" > "$work/.commands"
    awk -v p="$PROMPT" 'index($0, p) != 1 { sub(/^[ \t]+/, ""); print }' "$session" > "$work/.expected"

    (cd "$work" && "$SHELL_BIN" < .commands 2>&1) \
        | awk -v p="$PROMPT" '{ while ((i = index($0, p)) > 0) $0 = substr($0, 1, i - 1) substr($0, i + length(p)); sub(/^[ \t]+/, ""); print }' \
        > "$work/.actual"

    if diff "$work/.expected" "$work/.actual" > "$work/.diff"; then
        echo "PASS  $name"
        pass=$((pass + 1))
    else
        echo "FAIL  $name"
        cat "$work/.diff"
        fail=$((fail + 1))
    fi
    rm -rf "$work"
done

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
