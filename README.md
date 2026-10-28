# agent_runs: results of the E555 agents

This branch holds results, not code. It shares no history with main and is
never merged. Each campaign of an autonomous /goal agent owns one folder and
writes nothing else:

- `agent/checkpoints/<id>/`: campaigns of the beat-the-best kit
  (agent/autonomous_agent_mode.md), pushed by agent/checkpoint.sh;
- `agent/whirlpool/<id>/`: campaigns of the whirlpool kit
  (agent/whirlpool_agent_mode.md), pushed by agent/whirlpool/checkpoint.sh.

<id> is the campaign's UTC start time. Every commit names the code it ran
("Code: <sha> on <branch>"). Read a file with
`git show origin/agent_runs:agent/whirlpool/<id>/README.md`; a campaign
resumes from here with its kit's setup script, RESUME=agent_runs CAMPAIGN=<id>.
