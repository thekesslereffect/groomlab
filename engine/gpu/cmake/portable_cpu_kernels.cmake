# SPDX-License-Identifier: Apache-2.0
# slangc's C++ of the look kernels (IN) made the core's (OUT): its float transcendentals (the
# prelude's F32_sin, F32_pow...) the core's portable ones (groomlab::pm, through
# look_kernels_cpu_atomics.h's groomlab_pm_*), so the CPU engine rounds as the core's operators do on
# every platform; the prelude included by name (the include path's), not slangc's absolute path.
file(READ "${IN}" text)
string(REGEX REPLACE "F32_(sin|cos|tan|asin|acos|atan2|atan|sinh|cosh|tanh|exp2|exp|log2|log10|log|pow|cbrt)\\(" "groomlab_pm_\\1(" text "${text}")
string(REGEX REPLACE "#include \"[^\"]*slang-cpp-prelude\\.h\"" "#include \"slang-cpp-prelude.h\"" text "${text}")
file(WRITE "${OUT}" "${text}")
