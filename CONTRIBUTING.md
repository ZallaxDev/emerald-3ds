# Contributing

Thanks for your interest. A few rules keep this repository publishable.

## Branches and pull requests

[Guía rápida en español](#guía-rápida-en-español).

The contribution flow is:

```text
working branch -> PR -> integration -> PR -> dev -> PR -> main
```

| Branch | Purpose | Who integrates changes? |
| --- | --- | --- |
| Your working branch | One feature, fix or documentation task. Start from the latest `integration`. | Collaborators with Write can create branches and push normally. |
| `integration` | Shared area for reviewing and testing contributions together; it may contain unfinished or unstable work. | Collaborators with Write can review, approve and merge other collaborators' PRs. One approval is required. |
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
task; do not work directly on `integration`, `dev` or `main`.

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

### 2. Open a PR into integration

On GitHub, choose **Pull requests -> New pull request** and explicitly select:

- **base repository:** `ZallaxDev/pokeemerald-3Ds-dualscreen`;
- **base:** `integration` (GitHub may initially suggest `main`);
- **compare:** your working branch.

Explain the problem, the change, how you tested it and any known limitations.
Link the related issue when there is one. Use a draft PR while the work is not
ready. Request review from another collaborator with Write access.

A PR into `integration` requires **one approving review** from someone with Write
access or higher. You cannot approve your own PR. Read-only users' reviews do not
satisfy this requirement. New commits dismiss previous approvals, so request a
fresh review after updating the PR.

### 3. Review and merge another collaborator's PR

A collaborator with Write access can open **Files changed**, inspect and test the
change, then use **Review changes -> Approve** (or **Request changes** when fixes
are needed). Once the required approval is present, conflicts are resolved and
GitHub permits merging, a collaborator with Write can use **Merge pull request**,
**Squash and merge** or **Rebase and merge**, as appropriate for the change.

Approval and merge are separate actions; the author may merge their own PR after
another eligible collaborator approves it. Follow the repository's testing
requirements even when GitHub does not enforce a check as a merge requirement.

### 4. Keep your branch up to date and resolve conflicts

Run these commands **on your own working branch**, with a clean working tree:

```sh
git fetch origin
git merge origin/integration
# If there are conflicts: resolve them in the affected files, then:
# git add path/to/resolved-file
# git commit
git push
```

Run the relevant checks again and request a fresh approval. If you cannot resolve
the conflicts safely, ask another collaborator for help. Do not force push to a
shared integration or release branch. After a PR is merged, you may delete your
own working branch if it is no longer needed; start the next task on a fresh
branch from the latest `origin/integration`.

### 5. Promotion to dev and main is an administrator decision

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

- `integration`: PR required, one approval, stale approvals dismissed, no bypass.
  Direct pushes are blocked for administrators as well as collaborators.
- `main` and `dev`: updates and merges restricted to administrators. PRs are
  configured with zero mandatory approvals. Only `Repository admin` has
  **Always allow** bypass, so administrators can also make an exceptional direct
  push or merge without approval; PRs remain the normal documented workflow.
- All three branches block **force pushes and deletion**, including for
  administrators, through a separate ruleset with no bypass actors.
- Working branches have no additional ruleset restrictions: normal branch
  creation and pushes remain available to collaborators with Write.
- The rulesets do not enforce the source branch of a PR or automatically promote
  changes. Selecting the correct base and following this flow are team practices.

If GitHub rejects a push, check whether you targeted a protected branch and push
your work to your own branch instead. If merge is blocked, check the PR base,
required approval, new commits since approval and merge conflicts. A collaborator
being unable to merge into `dev` or `main` is expected; ask the administrator to
review the promotion rather than changing protections.

Contributors without Write access should fork the repository, create a working
branch from this repository's `integration` and open a cross-repository PR with
**base: integration**. Push to your fork, not to this repository. A collaborator
with Write or an administrator must review and merge it under the same rules.

The current rules can be inspected on the repository's
[Rules page](https://github.com/ZallaxDev/pokeemerald-3Ds-dualscreen/rules).

## Guía rápida en español

**Flujo:** rama de trabajo -> PR -> `integration` -> PR -> `dev` -> PR -> `main`.

| Rama | Cómo utilizarla |
| --- | --- |
| Tu rama de trabajo | Créala desde `origin/integration` para una sola tarea. Puedes hacer commits y push normalmente si tienes Write. |
| `integration` | Destino de los PR de colaboradores. Sirve para revisar y probar cambios juntos; puede ser inestable. |
| `dev` | El administrador decide qué cambios de `integration` pasan aquí. Los colaboradores no pueden hacer push ni merge. |
| `main` | Última versión publicada. Solo el administrador integra los cambios, normalmente desde `dev`. |

1. Con el árbol de trabajo limpio, ejecuta `git fetch origin` y crea tu rama con
   `git switch -c fix/tu-nombre-descripcion origin/integration`.
2. Realiza los cambios, comprueba lo que vas a incluir y ejecuta las pruebas
   correspondientes. Añade los archivos concretos, haz commit y publica tu rama
   con `git push -u origin fix/tu-nombre-descripcion`.
3. Abre un PR seleccionando **base: integration** y **compare: tu rama**.
   GitHub puede sugerir `main`: cambia el destino antes de crear el PR.
4. Explica qué has cambiado y cómo lo has probado. Pide revisión a otra persona
   con Write o a un administrador. Nadie puede aprobar su propio PR.
5. Hace falta **una aprobación válida**. Si añades nuevos commits, las
   aprobaciones anteriores se descartan y debes pedir otra revisión.
6. Una vez aprobado y sin conflictos ni otros bloqueos, una persona con Write
   puede hacer merge, incluido el autor del PR. Para revisar un PR ajeno, usa
   **Files changed -> Review changes -> Approve**.
7. El administrador decide cuándo promover `integration -> dev -> main`;
   integrar tu PR en `integration` no publica automáticamente una versión.

Para actualizar tu trabajo, ejecuta `git fetch origin` y
`git merge origin/integration` **desde tu propia rama**. Resuelve los conflictos,
haz commit si corresponde, vuelve a probar y ejecuta `git push`. Después pide
una nueva aprobación. Los ejemplos de comandos completos están arriba.

No se permiten pushes directos a `integration`, ni force pushes o eliminación
de `integration`, `dev` o `main`. El bypass administrativo de `dev` y `main`
permite excepciones al PR, pero no al bloqueo independiente de force push y
eliminación. Las ramas personales no tienen estas restricciones, aunque cualquier
colaborador con Write puede técnicamente escribir en ellas: coordínate con su autor.

Si no tienes Write, utiliza un fork y envía el PR al repositorio original con
**base: integration**. Si un PR está bloqueado, revisa su destino, la aprobación
y los conflictos; para promover a `dev` o `main`, contacta con el administrador.

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
- Open contribution pull requests against `integration`; administrator-only
  promotion PRs move selected changes to `dev` and then `main`.
- Run `make verify` and the builder's tests before opening a pull request.
- Test on hardware when you touch rendering, memory, timing or file I/O, and
  say what you tested in the pull request.

## AI-assisted contributions

Welcome, under the same standard as any other: you are responsible for what
you submit, it must be reviewed and tested, and prompts or transcripts do not
belong in the repository. See `AI_DISCLOSURE.md`.

