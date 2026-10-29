# Full-text pipeline optimization

The candidate builds on commit 23798003eeb4113babfcf55064d44793b95be838. It preserves the official benchmark, input data, SQL and expected results.

* Default-property calls to the built-in IK and BENG tokenizers can reuse a successful binary JSON result for identical input bytes, parser bytes, NULL flags and input collation. Arguments are evaluated on every call. Custom properties, invalid inputs and oversized values use normal tokenization. The eight-entry per-worker memo bounds retained payload to 256 KiB; cache allocation failure leaves SQL behavior unchanged. Dictionary hub identity and a generation counter invalidate the memo. Results are repacked for the current expression's JSON/LOB representation.
* Natural-language postings used only as a Boolean filter bypass per-document relevance dimension collection. The existing ordered merge, duplicate suppression, storage visibility and document lookup remain in use. Scoring, Boolean mode and additional relevance filters keep their original path.
* FTS auxiliary build tasks retain their requested parallelism. Storage range planning respects that degree even when a small input tablet has expensive generated-column processing. Storage range boundaries and the existing maximum parallel index still bound the split.
* FTS builds select an automatic degree of up to four workers, bounded by the configured CPU count (or detected CPUs when unset). Larger requested degrees remain intact. The automatic minimum applies to full-text tasks; ordinary and multivalue index tasks retain their existing degree policy.
* A direct COUNT(*) over a limited single-table view can skip the implicit full-text relevance order when the parent observes only cardinality. Explicit ordering, value consumers, joins, upper predicates and side effects retain the original path. Small consumer requests also bound document cache lookup batches.

The range-planning direction was informed by the Apache-2.0 implementation in https://github.com/oceanbase/seekdb/pull/2047 by buGanFor, commit 1223d0f7d5946325e2eb9bb87ce10bdbefb53cb3. Our memo uses exact keys and dictionary invalidation, rather than a hash-only key or time-based dictionary freshness assumption.

Native correctness, performance and platform scores must be recorded independently. This document does not claim an unmeasured speedup or a platform score.
