# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

# A consumer of uPCIe, built by check.sh against an installed uPCIe and through a wrap of the checkout
project('upcie-consumer', 'c', default_options: ['c_std=gnu11', 'warning_level=2', 'werror=true'])

executable('consumer', 'consumer.c', dependencies: dependency('upcie'))
