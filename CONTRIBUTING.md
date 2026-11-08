# Contributing

Thanks for your interest. A few rules keep this repository publishable.

## Branches and pull requests

[Guía rápida en español](#guía-rápida-en-español).

**For collaborators with Write:** You can merge your own branches directly into
`integration`, without a PR or anyone else's approval. You can also merge your
own PRs and review and merge PRs submitted by people without repository Write
access. All these merges target `integration`. Only administrators can push or
merge into `dev` and `main`.

**Administrator exception:** All three protected branches grant **Always allow**
bypass exclusively to **Repository admin**. Administrators can push or merge
without a PR or approval, and can bypass force-push and deletion restrictions.
Collaborators must follow the protected workflow below. Prefer PRs for traceability.

The contribution flow is:

```text
working branch -> direct merge OR optional PR -> integration -> PR -> dev -> PR -> main
```

| Branch | Purpose | Who integrates changes? |
| --- | --- | --- |
| Your working branch | One feature, fix or documentation task. Start from the latest `integration`. | Collaborators with Write can create branches and push normally. |
| `integration` | Shared development and testing area; it may be unstable. | Collaborators with Write can merge their branches directly, merge their own PRs and handle PRs from other collaborators or external contributors. No PR or approving review is mandatory. |
| `dev` | Administrator-controlled staging area for changes selected from `integration`. | Repository administrators only. |
| `main` | Latest released version. | Repository administrators only, normally through a PR from `dev`. |

Use descriptive branch names such as `feature/alex-touch-menu`,
`fix/alex-audio-crash` or `docs/alex-build-guide`. These names are a convention,
not an enforced naming restriction. Coordinate before pushing to someone else's
branch: Write access does not create private ownership of individual branches.

### 1. Create a working branch

For collaborators with Write access, the following example uses `origin` for this
repository. Replace the example branch name with your own.

```sh
git clone https://github.com/ZallaxDev/pokeemerald-3Ds-dualscreen.git
cd pokeemerald-3Ds-dualscreen
git fetch origin
git switch -c feature/alex-touch-menu origin/integration
```

If you already have a clone, start at `git fetch origin` with a clean working
tree (commit or stash any work first). Always create a separate branch for each
task. Develop on your working branch, then integrate it into `integration`.
Only administrators can update `dev` or `main`.

Make your changes, run the relevant checks described below and in
[the development guide](docs/DEVELOPMENT.md), then commit only the intended files:

```sh
git status
git add path/to/changed-file
git commit -m "Fix touch menu navigation"
git push -u origin feature/alex-touch-menu
```

Replace `path/to/changed-file` and the commit message with your actual changes.
Check `git diff --cached` before committing. Never include the files listed under
[Never commit](#never-commit).

### 2. Merge your branch directly into integration

A collaborator with Write can integrate tested work without a PR, another
person's approval or administrator involvement. From a clean working tree:

```sh
git fetch origin
git switch -c integrate/alex-touch-menu origin/integration
git merge --no-ff feature/alex-touch-menu
# Resolve conflicts, stage the resolved files and finish the merge if needed.
# Run the relevant checks on the merged result before pushing.
git push origin HEAD:integration
```

Use your actual working branch and a new name for the local merge branch. If
someone updates `integration` before your push, run `git fetch origin`, then
`git merge origin/integration` on your merge branch, resolve conflicts, test again
and retry `git push origin HEAD:integration`. Never force push over others' work.

Allowing direct Git merges also allows ordinary fast-forward pushes and direct
commits to `integration`: GitHub cannot distinguish those normal updates with
these rules. Prefer working branches and test the merged result before pushing.

### 3. Open an optional PR into integration

On GitHub, choose **Pull requests -> New pull request** and explicitly select:

- **base repository:** `ZallaxDev/pokeemerald-3Ds-dualscreen`;
- **base:** `integration` (GitHub may initially suggest `main`);
- **compare:** your working branch.

Explain the problem, the change, how you tested it and any known limitations.
Link the related issue when there is one. Use a draft PR while the work is not
ready. Review by another collaborator is welcome but optional.

A PR into `integration` requires **zero approving reviews**. Collaborators with
Write may merge their own PRs without another person's approval. GitHub does not
let authors approve their own PRs, but approval is not needed to merge here.
Adding commits does not introduce a mandatory approval or reapproval step.

### 4. Review and merge PRs from collaborators or external contributors

A collaborator with Write access can open **Files changed**, inspect and test the
change, then use **Review changes -> Approve** (or **Request changes** when fixes
are needed). Approval is optional. Once conflicts are resolved and GitHub permits
merging, a collaborator with Write can use **Merge pull request**,
**Squash and merge** or **Rebase and merge**, as appropriate for the change.

This includes PRs from forks submitted by people without Write access. Those
contributors cannot push or merge into this repository themselves; a collaborator
with Write or an administrator reviews and integrates their changes. The author
of a PR who has Write may also merge their own PR without approval. Follow the
repository's testing requirements even when checks are not enforced by GitHub.

### 5. Keep your branch up to date and resolve conflicts

Run these commands **on your own working branch**, with a clean working tree:

```sh
git fetch origin
git merge origin/integration
# If there are conflicts: resolve them in the affected files, then:
# git add path/to/resolved-file
# git commit
git push
```

Run the relevant checks again; no mandatory reapproval is required. If you cannot resolve
the conflicts safely, ask another collaborator for help. Do not force push to a
shared integration or release branch. After a PR is merged, you may delete your
own working branch if it is no longer needed; start the next task on a fresh
branch from the latest `origin/integration`.

### 6. Promotion to dev and main is an administrator decision

Merging into `integration` does **not** automatically publish a release or move
changes into `dev`. The administrator reviews and selects what is ready, normally
with **base: dev / compare: integration**, and later **base: main / compare: dev**.
Collaborators may propose these PRs, but only an administrator can merge them.

For these long-lived branch promotions, prefer a **merge commit** to preserve
shared ancestry. The administrator should inspect the complete diff and tests;
a promotion PR includes all differences between its source and destination, not
just the last contribution. Selection of only some changes needs a separate,
reviewed preparation branch.

### Protections and common blockers

- `integration`: collaborators with Write may merge directly, push normally,
  merge their own optional PRs and handle third-party PRs. No PR or approval is
  required. Force pushes and deletion remain blocked for collaborators.
- `main` and `dev`: updates and merges restricted to administrators. PRs are
  configured with zero mandatory approvals. Only `Repository admin` has
  **Always allow** bypass, so administrators can also make an exceptional direct
  push or merge without approval; PRs remain the normal documented workflow.
- All three branches block **force pushes and deletion** for collaborators.
  The separate protection ruleset also grants **Always allow** bypass exclusively
  to `Repository admin`, so administrators retain full control.
- Working branches have no additional ruleset restrictions: normal branch
  creation and pushes remain available to collaborators with Write.
- The rulesets do not enforce the source branch of a PR or automatically promote
  changes. Selecting the correct base and following this flow are team practices.

If GitHub rejects a normal push to `integration`, check your Write access and
whether the remote branch advanced; fetch and merge its latest changes before
retrying. Do not force push. If a PR merge is blocked, check its base, conflicts
and any other GitHub merge blockers. A collaborator
being unable to merge into `dev` or `main` is expected; ask the administrator to
review the promotion rather than changing protections.

Contributors without Write access should fork the repository, create a working
branch from this repository's `integration` and open a cross-repository PR with
**base: integration**. Push to your fork, not to this repository. A collaborator
with Write or an administrator handles its review and merge; no approving review
is mandatory.

The current rules can be inspected on the repository's
[Rules page](https://github.com/ZallaxDev/pokeemerald-3Ds-dualscreen/rules).

## Guía rápida en español

**Si tienes Write, puedes fusionar tus propias ramas directamente a `integration`
sin PR ni aprobación de otra persona. También puedes fusionar tus propios PR y
revisar y fusionar a `integration` los PR de terceros que no tienen permisos.**
No necesitas intervención del administrador para estas integraciones. Solo el
administrador puede hacer push o merge a `dev` y `main`.

**Flujo:** rama de trabajo -> merge directo o PR opcional -> `integration` -> PR
administrativa -> `dev` -> PR administrativa -> `main`.

| Rama | Cómo utilizarla |
| --- | --- |
| Tu rama de trabajo | Créala desde `origin/integration` para una sola tarea. Puedes hacer commits y push normalmente si tienes Write. |
| `integration` | Con Write puedes fusionar directamente tus ramas sin PR ni aprobación ajena, fusionar tus propios PR y gestionar PR de terceros sin permisos. Sirve para probar cambios juntos y puede ser inestable. |
| `dev` | El administrador decide qué cambios de `integration` pasan aquí. Los colaboradores no pueden hacer push ni merge. |
| `main` | Última versión publicada. Solo el administrador integra los cambios, normalmente desde `dev`. |

1. Con el árbol de trabajo limpio, ejecuta `git fetch origin` y crea tu rama con
   `git switch -c fix/tu-nombre-descripcion origin/integration`.
2. Realiza los cambios, comprueba lo que vas a incluir y ejecuta las pruebas
   correspondientes. Añade los archivos concretos, haz commit y publica tu rama
   con `git push -u origin fix/tu-nombre-descripcion`.
3. Puedes integrar sin PR usando los comandos de **Merge your branch directly
   into integration**: prepara el merge desde la última `origin/integration`,
   resuelve los conflictos, prueba el resultado y haz push a `integration`.
4. Si prefieres un PR, selecciona **base: integration / compare: tu rama**.
   Puedes hacer merge tú mismo: no necesitas aprobación ajena ni del administrador.
5. Si tienes Write, puedes revisar y fusionar a `integration` los PR cuyos autores
   no tienen Write. Esos autores deben usar un fork y no pueden hacer push ni
   merge en este repositorio por sí mismos.
6. Para revisar un PR ajeno puedes usar **Files changed -> Review changes ->
   Approve**, pero esa aprobación es opcional. Añadir commits no exige otra
   aprobación. Resuelve conflictos y prueba los cambios antes de hacer merge.
7. El administrador decide cuándo promover `integration -> dev -> main`;
   integrar tu PR en `integration` no publica automáticamente una versión.

Para actualizar tu trabajo, ejecuta `git fetch origin` y
`git merge origin/integration` **desde tu propia rama**. Resuelve los conflictos,
haz commit si corresponde, vuelve a probar y ejecuta `git push`. No hace falta
pedir aprobación. Los ejemplos de comandos completos están arriba.

Los colaboradores con Write pueden hacer merge directo y push normal a
`integration`, pero no force push ni eliminar `integration`, `dev` o `main`.
Permitir merges locales también permite commits directos a `integration`;
GitHub no distingue esas actualizaciones mediante estas reglas. Usa ramas de
trabajo y prueba el resultado antes de integrar. **Solo `Repository admin` tiene bypass
Always allow en los tres Rulesets**: el administrador conserva permiso para push
directo, merge sin PR ni aprobación, force push y eliminación en las tres ramas.
Puedes usar PR opcionales para mantener la trazabilidad. Las ramas personales no
tienen estas restricciones, aunque cualquier colaborador con Write puede
técnicamente escribir en ellas: coordínate con su autor.

Si no tienes Write, utiliza un fork y envía el PR al repositorio original con
**base: integration**. Un colaborador con Write puede encargarse de revisarlo y
fusionarlo. Si está bloqueado, revisa su destino y los conflictos; para promover
a `dev` o `main`, contacta con el administrador.

## Never commit

- ROMs, saves, `emerald3ds.pak`, or any file extracted or generated from a ROM
  (graphics, maps, audio, text, tables, voxel data);
- logs, screenshots from test runs, editor or AI-agent state, chat
  transcripts;
- build outputs (`*.3dsx`, `*.elf`, `3ds_port/romfs/` contents).

`python tools/release_audit.py --repo . --strict` must pass; CI runs it on
every pull request. New images or audio need a provenance entry in
`tools/release_audit_allow.toml` and must be your own work.

## Changes to the decompilation

The upstream tree is not part of this repository: its changes live in
`patches/pokeemerald/`. Prefer a hook in `3ds_port/` over editing a game
file, and keep game-side edits minimal and behind `PORT_BRIDGE` or
`PLATFORM_3DS`. To change a patch, edit the bootstrapped tree in
`build/upstream`, then regenerate the patches from it (`git diff` against the
pinned commit, one file per topic; see the headers of the existing patches).

## Code

- C99/GNU99. Native units build with `-Werror`.
- Match the surrounding code; comments explain *why*.
- Integrate contributions into `integration` with a direct merge or optional PR;
  only administrators promote selected changes to `dev` and then `main`.
- Run `make verify` and the builder's tests before opening a pull request.
- Test on hardware when you touch rendering, memory, timing or file I/O, and
  say what you tested in the pull request.

## Licence of contributions

By contributing you agree that your work is published under the project's
licence, CC BY-NC-SA 4.0 (`LICENSE-PORT.md`), with credit to you in the
history and the changelog.

## AI-assisted contributions

Welcome, under the same standard as any other: you are responsible for what
you submit, it must be reviewed and tested, and prompts or transcripts do not
belong in the repository. See `AI_DISCLOSURE.md`.
