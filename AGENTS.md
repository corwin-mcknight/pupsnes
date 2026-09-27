# Repository workflow

Before opening or updating a pull request for delivery, review the change and its validation results for unresolved findings, limitations, and deferred work. Create a GitHub issue for each actionable item that remains outside the PR's scope. Search existing issues first and reuse a matching issue instead of creating a duplicate. Include reproduction details or evidence, the intended scope, and clear completion criteria; distinguish confirmed defects from unverified hypotheses.

Link these follow-up issues in the PR description and relevant design documentation. Do not leave actionable findings only in a chat summary or PR caveat. Apply area and purpose labels, and record parent/sub-issue or blocking relationships when appropriate. Creating these follow-up issues is part of the authorized PR delivery workflow and does not require a separate confirmation.

Use `correctness` for hardware fidelity, `bug` for demonstrated failures, `regression` for previously working behavior, `validation` for diagnostic/comparison work, and `investigation` when evidence is needed before implementation. Combine these with relevant area labels such as `ppu`, `apu`, `input`, and `save-state`. Reserve `enhancement` for new capabilities rather than applying it to every issue.
