# Documentation

Start with the [project overview](../README.md), then the guide for your
language: [Python](../python/README.md), [R](../r/README.md) or
[JavaScript](../js/README.md).

## Use

| Page | Read it when |
| --- | --- |
| [Coming from cmdstanr](from-cmdstanr.md) | You have cmdstanr code and want the stanli equivalent of each call. |
| [Teaching with stanli](teaching.md) | You are setting up a class, including brms and Rethinking models. |
| [Fast mode](fast-mode.md) | You want more speed and can accept results that differ from CmdStan in the last bits. |
| [Within-chain parallelism](native-reduce-sum.md) | Your model uses `reduce_sum` and you want more than one thread per chain. |

## Trust

| Page | Read it when |
| --- | --- |
| [Testing](../TESTING.md) | You want to know what is checked, against what, and the known limits. |
| [Corpus status](corpus-status.md) | You want to know whether a particular model runs and matches CmdStan. Generated. |
| [Distribution coverage](coverage.md) | You want to know which densities, CDFs and transforms are supported. |
| [The lp constant](lp-constant.md) | Your `lp__` differs from CmdStan's by a constant. |
| [Benchmarks](benchmarks.md) | You want speed numbers and how they were measured. |

## Understand

| Page | Read it when |
| --- | --- |
| [How it works](how-it-works.md) | You want to know why an interpreter can be faster than compiled C++. |
| [Lowering walkthrough](lowering-walkthrough.md) | You want to follow three small models through every layer. |
| [Graph optimizations](../runtime/src/OPTIMIZATIONS.md) | You want the list of graph passes, their switches and their measured effect. |
| [Install size](install-size.md) | You want to know what makes up the shared library. |

## Contribute

| Page | Read it when |
| --- | --- |
| [Hacking on stanli](hacking.md) | You want to build, change, or verify a change. |
| [Runtime source map](../runtime/src/README.md) | You are looking for a file in the runtime. |
| [Releasing](../RELEASING.md) | You are cutting a release. |
| [Changelog](../CHANGELOG.md) | You want to know what changed between versions. |
| [Third-party licenses](../THIRD_PARTY_LICENSES.md) | You are redistributing stanli. |

Dated experiment reports and earlier benchmark tables live in
[notes/](../notes/README.md). Design history lives under
[internal/](internal/README.md). Neither is needed to use stanli.
