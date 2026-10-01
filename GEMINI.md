# Antigravity Workspace Guidelines & Rules

## Command Execution & Permissions
- **Use Authorized Standard Commands**: Strictly use standard system/CLI utilities (such as `git`, `systemctl`, `curl`, `ls`, `cat`, `grep`, `mkdir`, `rm`, `find`, `journalctl`).
- **No Custom Python in Shell**: Do NOT execute ad-hoc Python one-liners (`python3 -c "..."`) or temporary python scripts via `run_command`. Custom python invocations trigger user permission prompts (`Ctrl-K`) in the terminal.
- **Prefer Native Agent Tools**: Always use built-in tools (`view_file`, `replace_file_content`, `write_to_file`, `grep_search`, `find_by_name`, `list_dir`) for inspecting, modifying, and searching files instead of shell commands.
