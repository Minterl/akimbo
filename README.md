# Akimbo

Math (mostly ML) JIT written in freestanding* C with zero* dependencies.

\*it requires a C compiler and has a soft dependency on libc.

> [!WARNING]
> This project is in its absolute infancy, and as such, is yet not usable.

## Why, and what is it?

I really like the elegance and experience of systems like JAX, but not the fact that I have to pull in (and wait for) the entire LLVM toolchain and a Python interpreter to
build a simple neural network. The bet behind Akimbo is that one can do a lot of that, much faster, with a lot less code.

Akimbo aims to leverage useful mathematical abstractions to compile abstract math into powerful executable formats on the fly
in the least amount of time possible.

## Core Components

### The MIRV Meta-Compiler DSL

It's like a home-grown MLIR, but with a much smaller scope. It is both an IR description language (similar to MLIR's ODS) and
a declarative logic language with no inherent execution semantics. It is designed with first-class opaque host (C) types in mind,
allowing much of the compilation process to happen inside those types s.t the IR stream itself more so represents the topological
relationships between symbols, rather than their substance.

There are a million reasonable post-hoc justifications as to why I decided to build an entire DSL to do this, like that it removes boilerplate,
allows me to move slow parts of compilation offline, etc. The real reason is that it is the most enjoyable way for me to solve the problem of writing lots
of code that does roughly the same thing for different stages of the pipeline.

As of right now, the entire thing is [one C file](tools/mirv.c), and [an example](tools/ir_test.mirv).

### The Polyhedral Model (kinda)

Traditionally, the [polyhedral model](https://en.wikipedia.org/wiki/Polytope_model) involves bringing in ILP solvers like ISL, solving NP-complete problems, 
and discovering novel "permutations" of loop nests on the fly.

Akimbo adopts the concepts of formulating loop nests as convex polytopes and affine transforms, but skips on the ILP-based scheduling part. It also introduces the term "Address
Operator," which decouples the logical layout of a multi-dimensional array from its physical layout. This has prior art conceptually, but I haven't seen that name 
used specifically anywhere.

Here's what my version looks like in MIRV:

```
%loop_body: LoopBody = lambda (%induction_vector : Coordinate) {
    %y_coord:  Coordinate = ApplyAffineTransform(%induction_vector, %y_transform);
    %y_addr:   Address    = ApplyAffineAddressOperator(%y_addr_op, %y_coord);

    %x0_coord: Coordinate = ApplyAffineTransform(%induction_vector, %x0_transform);
    %x0_addr:  Address    = ApplyAffineAddressOperator(%x0_addr_op, %x0_coord);

    %x1_coord: Coordinate = ApplyAffineTransform(%induction_vector, %x1_transform);
    %x1_addr:  Address    = ApplyAffineAddressOperator(%x1_addr_op, %x1_coord);

    %x0: Scalar = Access(%x0_addr);
    %x1: Scalar = Access(%x1_addr);
    %cons_pair: ScalarConsPair = MakeScalarConsPair(%x0, %x1);
    %y: Scalar = ApplyScalarExpression(%scalar_expr, %cons_pair);

    YieldAccumulate(%y_addr, %y);
};

For(%D, %loop_body);
```

In Akimbo, the polyhedral model is a mathematical idea I think is quite elegant, and unifies problems like layout/iteration order locality optimization, processing element allocation,
high-level scheduling, and parts of kernel fusion/fission under one abstraction. 
I do not plan to actually analyze convex sets to the degree that something like [LLVM's Polly](https://polly.llvm.org/) does,
as much as that project is brilliant.

### The Base Layer

This is a term I stole from Ryan Fleury, who notably is responsible for the [RAD Debugger](https://github.com/EpicGames/raddebugger).

It describes the part of a project that pertains much less to the actual domain of the problem as much as being the standard library 
C does not have. If you want to see the benefits of this philosophy in code that runs on other people's computers, click the above link.

Akimbo's base layer is [here](include/akimbo/internal/base.h) and [here](include/akimbo/internal/base.c).

## Can I use it?

Because this project is currently a collection of somewhat disjoint ideas, no, you cannot use it or contribute to it in any meaningful way yet.

The goal is that this library will have *every* symbol prefixed s.t it is safe to embed even in an existing translation unit,
similar to the STB libraries, but without the headache of having to maintain a 10,000 line header file.

## What's next?

Akimbo will be able to emit a loop nest in C, and then compile and run a network that classifies the MNIST dataset.

Other future objectives:
 - Autograd
 - Compilation down to PTX & SPIR-V
 - Native x86 machine code generation
