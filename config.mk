# config.mk — local build configuration (edit to match your machine).
#
# The Fractal Trie (FT) is NOT vendored here; it lives in userspace-rcu and is
# under active development.  We keep our OWN clone of it (a checkout of the
# fractal-trie-dev branch) under $(URCU_BUILD) and build it in-tree, so the FT
# we link is a consistent committed state we control — decoupled from the live
# source tree (which may be mid-refactor).  `make urcu` clones + builds it.

# userspace-rcu repository to clone our FT checkout from (offline --local).
URCU_UPSTREAM ?= /home/efficios/git/userspace-rcu
# Branch to check out and build.
URCU_BRANCH   ?= fractal-trie-dev

# Our own liburcu clone + in-tree build (created by `make urcu`).  Headers live
# under $(URCU_BUILD)/include, libraries under $(URCU_BUILD)/src/.libs.
URCU_BUILD ?= $(CURDIR)/urcu-build

# CFLAGS used when building liburcu itself.  -march=native enables the
# popcount/AVX paths the FT uses.
URCU_CFLAGS ?= -O2 -DNDEBUG -mavx2 -mbmi2 -mbmi -mpopcnt -msse4.2 -march=native

# Extra Fractal Trie feature flags for the *benchmark* compile.
# NOTE: -DFEATURE_FT_QP -DFEATURE_FT_POPCOUNT_NODE are LEGACY no-ops in the
# current FT source (that design is now the default); kept here only to mirror
# the original ft_benchmark build scripts.  The live feature gates are
# FEATURE_FT_COMPRESS / FEATURE_FT_SKIP_COMPRESSED, both on by default.
FT_FEATURES ?= -DFEATURE_FT_QP -DFEATURE_FT_POPCOUNT_NODE

# Benchmark compiler / optimization (-march=native -mpopcnt matches the bench).
CC      ?= gcc
OPTFLAGS ?= -O2 -DNDEBUG -march=native -mpopcnt

# --- Multithreaded (bind9) benchmarks ------------------------------------
# The bind9 "lookup names" MT scaling test (load-names) and the standalone
# bench_scale_rw_bind9 test both need BIND9's QP-trie.  We clone a clean
# upstream bind9 at a pinned commit (offline, from BIND9_UPSTREAM's .git),
# overlay our tests/bench files, and build linking our own liburcu.

# bind9 repository to clone from (offline --local clone of its committed state).
BIND9_UPSTREAM ?= /home/efficios/files/fractal-trie/bench-comprehensive/bind9
# Pinned bind9 commit the overlay was written against.
BIND9_COMMIT   ?= 4197958d03eede352b71e07994f369cedde3ba8b
# Where we create our clean clone + overlay + build (gitignored).
BIND9_SRC      ?= $(CURDIR)/bind9-src
