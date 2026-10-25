# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE HARNESS FLOOR (fuzz/coverage.sh). Reads the `llvm-cov show` of one harness source, `LINE|COUNT|source`, and
# prints one line for every abort() in it:
#
#   <file>:<guard line>:<abort line>:<count>:<the guard's first source line>
#
# The guard is the statement that decides whether the abort runs: the `if` it is the body of, or the statement it ends
# when it shares that line. <count> is how often the seed corpus executed the guard's first line; 0 is a property no
# seed tests. The abort's own count is not it: on a corpus that holds, no abort runs. Comments are stripped first, so an
# abort() named in prose is not a site.
BEGIN { FS = "|" }
{
    ln = $1 + 0
    cnt = $2
    gsub(/[ \t]/, "", cnt)
    src = $0
    sub(/^[^|]*\|[^|]*\|/, "", src)
    code = src
    if (incomment) {
        if (code ~ /\*\//) { sub(/^.*\*\//, "", code); incomment = 0 }
        else code = ""
    }
    while (match(code, /\/\*([^*]|\*+[^*\/])*\*+\//))
        code = substr(code, 1, RSTART - 1) substr(code, RSTART + RLENGTH)
    if (code ~ /\/\*/) { sub(/\/\*.*$/, "", code); incomment = 1 }
    sub(/\/\/.*$/, "", code)
    gsub(/^[ \t]+|[ \t]+$/, "", code)
    C[ln] = code; N[ln] = cnt; S[ln] = src
    if (code ~ /(^|[^A-Za-z0-9_])abort\(\);/) site[++ns] = ln
}

# the first line of the statement that ends on line l
function stmt_start(l) {
    while (l > 1 && C[l - 1] != "" && C[l - 1] !~ /[;{}:]$/) l--
    return l
}

END {
    for (i = 1; i <= ns; i++) {
        l = site[i]
        s = (C[l] ~ /^abort\(\);/) ? stmt_start(l - 1) : stmt_start(l)
        t = S[s]
        gsub(/^[ \t]+/, "", t)
        printf "%s:%d:%d:%s:%s\n", file, s, l, N[s], t
    }
}
