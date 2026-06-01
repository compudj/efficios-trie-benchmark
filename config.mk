# config.mk — local build configuration (edit to match your machine).
#
# The Fractal Trie (FT) is NOT vendored here; it lives in userspace-rcu and is
# under active development.  We build our OWN out-of-tree liburcu from that
# source into $(URCU_BUILD) so the FT we link always matches the current
# source (e.g. picks up new API like cds_ft_compact).  We never modify the
# userspace-rcu source tree itself.

# Path to the userspace-rcu *source* tree to build the Fractal Trie from.
URCU_SRC   ?= /home/efficios/git/userspace-rcu

# Our own out-of-tree liburcu build directory (created by `make urcu`).
# Keep this OUTSIDE the userspace-rcu source tree.  Default: repo-local.
URCU_BUILD ?= $(CURDIR)/urcu-build

# CFLAGS used when building liburcu itself (mirrors the upstream build-bench
# build).  -march=native enables the popcount/AVX paths the FT uses.
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
