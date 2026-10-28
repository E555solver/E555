# Strategy ledger

Measured before this campaign (PROJECT_E555.md 9.2, 9.3); re-test only with a new idea:
- CP-SAT windows around the breaks of polished boards (457-463): already optimal
  (78/78 4x4 windows); exact regions of 48-102 cells gained nothing.
- diver --reopen auto on the seven first 463s of data/best_463.csv, 120 s each:
  the first gained 2 (that board is also the root of data/best_465.csv), the
  other six gained nothing.
- Best measured reopen setting: --copies 8 --end_dive 1000 --end_polish 10000;
  it kept gaining from 15 s to 120 s per board.

## Families tried in this campaign
