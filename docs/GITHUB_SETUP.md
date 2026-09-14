# GitHub setup for a separate VTuber account

## 1. Create a dedicated SSH key

```bash
ssh-keygen -t ed25519 -C "vtuber-github" -f ~/.ssh/id_ed25519_vtuber
cat ~/.ssh/id_ed25519_vtuber.pub
```

Add the public key to the VTuber GitHub account under **Settings → SSH and GPG keys**.

## 2. Add an SSH host alias

Add to `~/.ssh/config`:

```sshconfig
Host github-vtuber
    HostName github.com
    User git
    IdentityFile ~/.ssh/id_ed25519_vtuber
    IdentitiesOnly yes
```

Test:

```bash
ssh -T git@github-vtuber
```

## 3. Configure identity only inside this repository

```bash
git config user.name "YOUR_VTUBER_NAME"
git config user.email "YOUR_GITHUB_NOREPLY_EMAIL"
```

## 4. Add the remote

```bash
git remote add origin git@github-vtuber:YOUR_VTUBER_ACCOUNT/bokis-twitch-chat-plugin.git
git push -u origin main
```

## 5. GitHub repository settings

Recommended after first push:
- Keep the repository private initially.
- Add a ruleset for `main` requiring CI to pass.
- Disable force pushes to `main`.
- Enable Dependabot for GitHub Actions.
- Before public binary releases, pin third-party GitHub Actions by full commit SHA.
- After the release pipeline is stable, enable immutable releases.
