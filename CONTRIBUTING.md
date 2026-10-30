# Contributing to NFS Carbon · Browser Edition

Focused fixes and reproducible bug reports are welcome. Please keep each pull request focused on one change.

## Branch and approval rules

- Contributors must submit changes through a pull request targeting `main`. Direct pushes to `main` are blocked for contributors.
- Every contributor pull request needs approval from the repository owner, **@metinjakupi**. Approval from another contributor does not replace the owner's approval.
- New reviewable commits dismiss previous approvals. Ask @metinjakupi to review the updated pull request again.
- Resolve all review conversations before merging.
- Only @metinjakupi has an exception for direct pushes to `main`. Force pushes and deletion of `main` are blocked for everyone.

The [CODEOWNERS file](.github/CODEOWNERS) assigns all files to @metinjakupi. The repository rules require code-owner approval.

## 1. Prepare your copy

If you do not have write access, click **Fork** on this repository to create a copy under your GitHub account. Then clone your fork:

```sh
git clone https://github.com/YOUR-USERNAME/nfs-crb.git
cd nfs-crb
git remote add upstream https://github.com/metinjakupi/nfs-crb.git
git fetch upstream
git switch -c fix/describe-your-change upstream/main
```

Replace `YOUR-USERNAME` with your GitHub username. Replace `fix/describe-your-change` with a short branch name that describes your work.

If you already have write access, clone the original repository and create a separate branch:

```sh
git clone https://github.com/metinjakupi/nfs-crb.git
cd nfs-crb
git switch -c fix/describe-your-change
```

## 2. Make and check your change

1. Follow the [development instructions](README.md#development) for local setup and builds.
2. Make the smallest change that solves the problem. Avoid unrelated formatting or cleanup.
3. Run the relevant checks or build for the area you changed. For a visible change, check the affected screen or game behavior.
4. Record what you tested and the result. If you could not run a check, explain that in the pull request.
5. Review your diff before committing:

```sh
git diff
git status
git add path/to/changed-file
git commit -m "Describe the change"
git push -u origin fix/describe-your-change
```

Replace `path/to/changed-file` with the files you changed. Add each intended file explicitly.

Do not commit original game files, generated game builds, personal saves, passwords, tokens, or other credentials. Local builds require your own compatible PC installation of Carbon 1.4; see the README for details.

## 3. Open a pull request

1. Open [the original repository](https://github.com/metinjakupi/nfs-crb) on GitHub.
2. Select **Pull requests → New pull request**. If you used a fork, select **compare across forks**.
3. Set the base repository to `metinjakupi/nfs-crb` and the base branch to `main`. Set the head repository to your fork, or the original repository if you have write access. Select your change branch as the compare branch.
4. Review the diff. Confirm that the pull request contains only your intended changes.
5. Enter a clear title and a description using the example below. Create a draft pull request if the work is still in progress.
6. Select **Create pull request** when the change is ready for review.

Suggested description:

```md
## What changed
Describe the problem and how this change solves it.

## Verification
- Checks or builds run and their results:
- Manual steps used to verify the change:
- Checks you could not run and why:

## Related issue
Link an issue, if applicable.
```

Add screenshots or a short recording when they help explain a visible change. Do not include private information.

## 4. Get approval from @metinjakupi

1. Mark the pull request **Ready for review** if you opened it as a draft. GitHub uses CODEOWNERS to request the owner's review automatically.
2. Check the **Reviewers** section for @metinjakupi. If the review was not requested and GitHub lets you choose a reviewer, request @metinjakupi. Otherwise, leave a comment asking @metinjakupi to review.
3. Respond to review comments. Commit any requested fixes and push them to the same branch; GitHub updates the existing pull request.
4. Request another review after new commits. Previous approval may have been dismissed.
5. Wait for @metinjakupi's approval and resolve all review conversations. Leave the final merge to @metinjakupi unless the owner asks you to merge.

A comment such as "looks good" is not a formal approval. The owner must submit an **Approve** review through GitHub.

## Reporting a bug

Use **Send report** in the game, or [open an issue](https://github.com/metinjakupi/nfs-crb/issues). Include the browser, operating system, graphics card, steps to reproduce, expected result, and actual result. Do not attach game files, personal saves, or credentials.
