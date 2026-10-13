---
name: git-commit-style
description: Define consistent Git commit message conventions for the project. Use when creating commits, reviewing commit messages, or generating AI-assisted commits.
---

# Git Commit Style

## Objective
Keep Git history readable, searchable, and consistent across all contributors and AI assistants.

## Global Constraints
Apply these constraints while using this skill.

1. Follow the established commit conventions; do not introduce alternative styles.
2. Keep commit messages factual and concise; avoid vague or overly generic descriptions.
3. Respect the existing project commit history as the reference for tone and granularity.

## Commit Message Format

Full format:
```
<type>(<scope>): <short description> — <detailed body>
```

Simplified format for trivial changes:
```
<type>(<scope>): <short description>
```

## Type

Use lowercase English types, compatible with Conventional Commits.

| Type | Meaning | Example |
|------|---------|---------|
| `feat` | New feature | `feat: ScriptCardPainter 动态布局引擎` |
| `refactor` | Code refactoring (no bug fix, no feature) | `refactor: ElaMultiComboBox Fluent UI` |
| `fix` | Bug fix | `fix(ElaAppBar): 修复 WM_GETMINMAXINFO 多屏 DPI 缩放错误` |
| `docs` | Documentation changes | `docs: 项目文档 + Memory Bank + Agent Skills` |
| `chore` | Miscellaneous (build, tooling, deps) | `chore(ElaWidgetTools): 移除冗余的 Q_SIGNAL 与 virtual 修饰符` |
| `revert` | Revert a previous commit | Generated automatically by `git revert` |

## Scope (optional)

- Component or module name, placed in parentheses after the type, with the first letter capitalized.
- Examples: `(ElaAppBar)`, `(ScriptPage)`, `(painter)`, `(delegate)`, `(ElaWidgetTools)`
- Omit the scope when the change spans multiple components.

## Language and Separator

- Both the subject line and body must use **Simplified Chinese** (简体中文). The `type` and `scope` tokens stay in lowercase English.
- Separate the short description from the detailed body with an **em dash (—)** surrounded by a single space on each side.
- Historical English commits may remain; do not rewrite them only to change the language.

## Style Rules

- ❌ Do not use emojis (✨ ♻ 📝 etc.)
- ✅ Use English type names (e.g., `refactor`), never Chinese equivalents
- ✅ Write the short description and body in Simplified Chinese
- ✅ A single space must follow the colon `:` that separates the type/scope and the description
- ✅ Use bullet points (`-`) for each change point in the body

## Examples

```
refactor(ElaPopupBase): 重构 Slide 动画引擎 — 方向感知偏移 + 位标志修复 + 透明度保护

- 修复误用按位或代替按位与、导致所有动画分支无条件执行的问题
- 修复 Fade 动画操作 windowOpacity 却被 QGraphicsOpacityEffect(opacity=0) 遮挡的问题
- 将 Slide 动画重构为方向感知
- 新增 AnimePosition 枚举以允许外部覆盖方向
- 新增 ElaPopupBasePrivate 私有实现头（PIMPL 分离）

feat(delegate): 实现 Delegate 布局缓存，避免 paint() 中重复计算布局

- CardLayoutCacheManager 单例，四元组缓存键
- sizeHint/paint/editorEvent 均从缓存读取
- 修正布局偏移；动画不污染缓存

fix(ElaAppBar): 修复 WM_GETMINMAXINFO 多屏 DPI 缩放错误

- 多 DPI 环境下 WM_GETMINMAXINFO 回调坐标计算错误
- 回退到 primaryScreen 的 logicalDotsPerInch
```

## Commit Granularity

- Each commit must represent **one independent logical change**.
- A single commit should touch **no more than 31 files** and stay within **500–800 lines** of change (split if larger).
- **Buildable**: every commit must compile and pass basic sanity checks on its own.

## Commit Message Checklist

1. Type is correct and matches the nature of the change.
2. Scope is included when the change is limited to a single component; omitted when cross-cutting.
3. Short description is in Simplified Chinese, concise, and starts with a verb or noun describing the change.
4. Em dash separator is present when a body exists.
5. Body uses `-` bullet points for each logical change detail.
6. No emojis or non-standard decorations.
7. Commit size respects file and line limits; large changes are split logically.
8. The commit builds independently.

---

*This specification is derived from the project’s recent commit history and applies to all contributors and AI coding assistants.*