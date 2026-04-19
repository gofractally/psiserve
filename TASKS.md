# Agent Tasks

## Setup (first time on a new machine)

Clone the private tasks repo as a sibling to this repo:

```bash
git clone git@github.com:bytemaster/psiserve-tasks.git ../psiserve-tasks
```

Then verify your git identity is set correctly before making any commits:

```bash
git config --global user.name "Daniel Larimer"
git config --global user.email "dan@fractally.com"
```

## Your Assignment

1. Find your agent file: `../psiserve-tasks/.agents/<your-agent-name>.md`
2. Read your current assignment and coordinator notes there
3. Follow the issue ID to `.issues/<id>-*.md` in this repo for full context and acceptance criteria
4. Update the `## Agent Status` section in your agent file as you complete work
5. Push your status updates: `git -C ../psiserve-tasks add -A && git -C ../psiserve-tasks commit -m "status update" && git -C ../psiserve-tasks push`

## Staying Current

```bash
git -C ../psiserve-tasks pull   # check for new assignments
git pull                         # pick up new issues and code
```
