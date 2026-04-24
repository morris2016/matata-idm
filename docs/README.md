# docs/ — GitHub Pages source

These three pages get hosted for the Chrome / Edge / Firefox store
listings:

| File           | URL after Pages is enabled |
|----------------|----------------------------|
| `index.md`     | `https://morris2016.github.io/matata-idm/`         |
| `privacy.md`   | `https://morris2016.github.io/matata-idm/privacy`  |
| `support.md`   | `https://morris2016.github.io/matata-idm/support`  |

## Publishing

From the repository root, once the code is committed:

```
git push -u origin main
```

Then in the GitHub web UI:

1. Repo → **Settings** → **Pages**
2. **Source**: `Deploy from a branch`
3. **Branch**: `main`  — **Folder**: `/docs`
4. Save. First build takes ~60s.

The URLs above come live as soon as the Actions / Pages build finishes
(green check on the Pages settings tab).

## Editing

These files are GitHub-Flavoured Markdown with Jekyll front matter
(`---` block at the top). GitHub renders them automatically; no local
Jekyll install needed. Commit to `main` and Pages rebuilds within a
minute.

## Swapping the theme

`_config.yml` sets `theme: jekyll-theme-cayman`. Any
[supported theme](https://pages.github.com/themes/) works — just
change the name.
