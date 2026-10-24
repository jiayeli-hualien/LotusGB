# Documentation writing guide

Use the Minto Pyramid Principle as a default for explanatory project documents:
lead with the answer or current state, group the main supporting points, and
then provide the evidence and details. The goal is to help a reader understand
the important point before reading the full history or implementation detail.

## Organize information by purpose

- **README:** explain what LotusGB is, what it currently supports, how to build
  and test it, and where to find the detailed status and roadmap.
- **Roadmap:** show the active route, acceptance criteria, and deferred work
  before recording completed milestones.
- **Technical guides:** start with scope and current behavior, then describe the
  relevant model or interface, validation evidence, known limits, and sources.
- **Procedures:** list prerequisites, commands or actions in execution order,
  expected results, and troubleshooting. Keep the steps chronological.
- **History and investigation notes:** preserve the event timeline, but label
  old results as historical and link to the current conclusion.
- **Reference tables:** retain tables when they make mappings, test coverage,
  or status easier to scan.

## Make claims easy to verify

- Distinguish behavior that is implemented, covered by tests, observed in a
  game, and verified against hardware. These are different levels of evidence.
- Name the relevant test or ROM, platform, and date when that context affects
  the claim. Link to primary specifications or test sources when available.
- Mark an inference as an inference; do not present an emulator comparison as
  direct hardware evidence.
- Keep current status in one canonical document and link to it from summaries.
  Update related links and test counts when evidence changes.
- Keep each paragraph focused on one point. Use headings that state the topic
  or conclusion so readers can scan the page.

## Apply the guide with judgment

This is a default structure, not a requirement to force every file into the
same template. Keep a small test-ROM README short and operational, and preserve
the ordering needed to follow an investigation. For a narrow code change,
update the affected documentation and links without turning it into a rewrite
of every document.
