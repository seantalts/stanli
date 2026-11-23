# Social feedback triage — 2026-11-23

Two reports merit reproduction: custom covariance models failing with a
low-level error, and a 500-subject mixed-effects model sampling more slowly
than CmdStanR. These are public user reports, not locally reproduced defects
or regressions established against an earlier Stanli revision.

Repository baseline: fetched `origin/main`,
`0eb880b9b9579cd28ca64c3afa4257cba9adca1e`. The clean worktree fast-forwarded
from `9ba6f89522fadd08319d137d0b2eb21bef04c4d7` without conflicts; the
remote-default ancestry check passed. No runtime changes or benchmarks were
made for this collection.

## Reports to reproduce

Tracked on GitHub as [#656](https://github.com/seantalts/stanli/issues/656)
and [#657](https://github.com/seantalts/stanli/issues/657). Both were filed
on 2026-11-23 with the source reports and missing reproduction details.
Separate subagent investigations started from the baseline above; neither
report is considered resolved.

- [#656 structural search](2026-11-23-issue-656-round2.md): 306 valid
  synthetic cases, 153 exact-source CmdStan point comparisons, and a
  standalone custom-covariance preparation/memory cost gap. The low-level
  error was not reproduced; the cost gap can be investigated without the
  original model.
- [#657 matched native and standard-fit comparison](2026-11-23-issue-657-native-comparison.md):
  eleven synthetic cells and one converged 500-subject fit show no slowdown
  on the measured host. Whole-workflow R memory is higher in Stanli;
  output/materialization boundaries and numerical limits are documented.
- [#657 historical-release check](2026-11-23-issue-657-historical-runtime.md):
  bounded 0.27.0/current native comparisons, with noisy small shifts and no
  claim about the reporter's installed version.
- [#656 first investigation](2026-11-23-issue-656-investigation.md):
  custom covariance/Cholesky diagnostics match CmdStan within 1 ULP and
  invalid matrices reject; the original error remains unreproduced.
- [#657 first investigation](2026-11-23-issue-657-investigation.md):
  reproducible 10/50/500-subject surrogate and retained numerical evidence;
  timing interfaces differ and full inference comparison is unfinished.

An additional direct user request is tracked separately as
[#658](https://github.com/seantalts/stanli/issues/658): support
`pathfinder_init` in R's `sample_walnuts()`. A third subagent owns that
implementation and its R interface tests. The [implementation and validation
record](2026-11-23-issue-658-pathfinder-walnuts.md) describes the completed
local change and remaining integration validation. This request did not come
from the social-media search.

### 1. Custom covariance / Cholesky models fail

- **Reporter/date:** Richard McElreath, 2026-11-22.
- **Source:** [failure report](https://bsky.app/profile/rmcelreath.bsky.social/post/3n2vgbotnl22z),
  [thread root](https://bsky.app/profile/rmcelreath.bsky.social/post/3n2vdifbn222q).
- **Observed:** some models that construct custom covariance matrices and
  take their Cholesky factors do not work. The reporter describes C-level
  bit-length errors. This is not an exact exception transcript, and the
  report does not establish which layer fails.
- **Context:** the successful examples are ordinary varying-slope models;
  the thread reports about 2.2x speedup on an M3 Max for roughly
  1,000-dimensional posteriors. The successful runs do not resolve the
  covariance failures.
- **Status:** unresolved from the inspected thread. No matching issue was
  identified by repository issue searches for `cholesky` and `bit length`;
  broad index matches are insufficient to establish a duplicate.
- **Needed:** generated Stan source, minimal data, Stanli package/runtime
  versions, full error and traceback, and the failing phase. The M3 Max
  context belongs to the successful benchmark; the failing case's platform
  still needs confirmation. Compare the same model with CmdStan before
  attributing the problem to a Cholesky kernel or compiler pass.

### 2. Mixed-effects model loses its speed advantage at 500 subjects

- **Reporter/date:** Demetri (`phdemetri.bsky.social`), 2026-11-17.
- **Sources:** [500-subject report](https://bsky.app/profile/phdemetri.bsky.social/post/3n2jpadku3s2i),
  [ten-run follow-up](https://bsky.app/profile/phdemetri.bsky.social/post/3n2js7xifgc27).
  [A later reply](https://bsky.app/profile/frodsan.bsky.social/post/3n2tzlqmerc2s)
  points to this case as a counterexample to universal speedup claims.
- **Model description:** one random intercept, one random slope, 500 unique
  subjects; the reporter increased the sample size. The number of
  observations is not given in the inspected posts.
- **Evidence:** the follow-up screenshot labels these as means ± standard
  errors, in seconds, over ten runs:

  | Interface | Build | Sampling | Total |
  | --- | ---: | ---: | ---: |
  | CmdStanR | 14.75 ± 0.13 | 71.74 ± 0.81 | 86.50 ± 0.93 |
  | Stanli | 0.44 ± 0.003 | 112.46 ± 1.43 | 112.90 ± 1.43 |

  These are transcribed from the original screenshot, not fresh timings.
  Their mean ratios imply about **1.57x sampling time** and **1.31x total
  time** for Stanli. The earlier single run shows the same direction
  (118.5 versus 76.7 seconds sampling; 119.0 versus 93.1 total).
- **Status:** unresolved from the inspected thread; no model code, version,
  hardware, sampler configuration, leapfrog counts or ESS is supplied there.
  A repository issue search for `500 subjects` found no match. This does not
  establish that the report is new or unrelated to existing work.
- **Needed:** exact generated program/data, versions/builds, seeds,
  chains/threads, warmup/draw counts and diagnostics. First distinguish
  slower gradients from additional sampler work or output costs. Repeat on
  current releases with matched configurations and compare ESS per second;
  do not infer a kernel or memory cause from the screenshot alone.

## Older reports and related context

| Thread | Interpretation and disposition |
| --- | --- |
| [JsonGeller, Nov 17: mixed models not benefiting as much](https://bsky.app/profile/jgeller1phd.bsky.social/post/3n2jkqkvl222z) | Refers to Dominique Makowski's cogmod work. [Makowski's reply](https://bsky.app/profile/dominiquemakowski.bsky.social/post/3n2jsmeaxhk2g) says most issues were solved. [Issue #455](https://github.com/seantalts/stanli/issues/455) is closed; its [Oct 30 explanation](https://github.com/seantalts/stanli/issues/455#issuecomment-6529441639) attributes the Linux DDM-4 gap to thread-local autodiff access and records its fix in 0.22.0 / PR #470. Treat this as historical evidence, not a new confirmed defect. The reply also changes sampler to nutpieR, so its broad speedup claim is not a matched Stanli-runtime comparison. |
| [Yaniv Abir, Nov 16: model needed rewriting for reduce_sum](https://bsky.app/profile/yanivabir.bsky.social/post/3n2gro4yab22r) | Reports a successful speedup, with model rewriting and concurrent laptop use confounding the comparison. Useful support/workload lead, but no failure or current performance defect is established. Related [#571](https://github.com/seantalts/stanli/issues/571) and [#577](https://github.com/seantalts/stanli/issues/577) are closed; the post alone does not establish a connection to either. |
| [Max Scholz, Nov 17: patched Stanli/walnutpie speedups and contribution difficulty](https://bsky.app/profile/scholzmx.bsky.social/post/3n2jo2em2ys2w) | Potential contributor/performance lead: reports about 28x ESS/s on Eight Schools with personal patches and difficulty contributing them. No patch link or controlled evidence is supplied in that post. Do not treat it as a reproducible runtime defect. |

The Nov 15 discussion of postprocessing, ADREPORT/REPORT, and difficulties
with recent Math-library updates follows a recommendation to try **RTMB**.
It should not be collected as evidence that Stanli itself has those defects.

## Signed-in Twitter/X review

Browser access and X sign-in were restored later on 2026-11-23. The inspected
search results and relevant conversations added **no new actionable bug or
slowdown report** to the two Bluesky items above.

| Thread | Finding |
| --- | --- |
| [Andy Timm, Nov 17](https://x.com/Andy_Timm/status/2122679137500631530), [follow-up](https://x.com/Andy_Timm/status/2122679141007073765), [documentation links](https://x.com/Andy_Timm/status/2122679143532048498) | Positive performance report, including ESS/s comparisons using nutpieR and a qualitative comparison with custom kernels. The inspected three-post conversation contains no failure or slowdown report. This overlaps the earlier Bluesky evidence rather than adding a new issue. |
| [Archana Burra, Aug 31](https://x.com/archanaburra/status/2094286034171494679) | Announcement/support thread. The visible reply contains no software bug or performance report. |

This is a bounded search result, not a claim that X contains no other reports.

## Search coverage and limits

- Collected on 2026-11-23 through Bluesky's unauthenticated
  `https://api.bsky.app/xrpc/` API. `app.bsky.feed.searchPosts` with
  `q=stanli`, `sort=latest`, `limit=100` returned 89 hits and no continuation
  cursor; many hits refer to unrelated uses of the name. A
  `q=seantalts/stanli` search returned eight hits, all already encountered.
- Followed reply/root threads with `app.bsky.feed.getPostThread`
  (`depth=20`, `parentHeight=10`), inspected quote-post results with
  `app.bsky.feed.getQuotes`, followed the quoted Demetri report, inspected
  its original images, and checked his recent author feed. This reaches
  replies and quoted reports that omit the word Stanli. It is not proof of
  exhaustive platform coverage: search indexing, deleted posts, moderation,
  and thread API limits can hide content.
- Cross-checked GitHub issue titles/bodies and #455's comments on the
  collection date. Closed issue status is not fresh local verification.
- **Twitter/X was reviewed directly after sign-in.** Initial public search
  returned HTTP 403 and browser discovery failed; those access problems were
  subsequently resolved. In signed-in Chrome, inspected Latest results for
  `stanli`, then narrowed the noisy name matches to
  `stanli (Stan OR brms OR CmdStan OR bug OR slow OR compiler OR sampler)`.
  The narrowed results reached July 29, before the August announcement;
  the software-related results were the threads listed above. Also checked
  Latest results for `url:github.com/seantalts/stanli`, which displayed three
  posts from those same two threads. Inspected their conversations. Search
  indexing, ranking, missing keyword/link matches, private or deleted posts,
  and unseen replies remain coverage limits.
- At the user's request, GitHub issues #656 and #657 were subsequently
  published and separate investigations started. No social-media messages
  or PRs were published, and no reporters were contacted. The reproduction
  requests above are collected next steps, not messages already sent.
