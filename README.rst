Torq QEMU Emulation
===================

This repository contains a fork of QEMU that can be used to emulate the Torq hardware found
in Synaptics Astra SL 2610.

The qemu device model for TORQ calls an external device model server. This device model server
can either use a host implementation or a FPGA backed implementation.

.. warning::

    This fork of QEMU is specifically tailored for the Torq hardware and is intended for
    Torq compiler/runtime testing purposes only. The emulation is doesn't replicate the actual 
    behavior of the full Astra SoC nor the Coral board. The emulation of the Torq hardware is 
    also limited to features used by the compiler and doesn't fully reflect the actual Torq 
    hardware behavior.

    
