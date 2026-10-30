# Upstream workflows (disabled)

These are Nonary/Vibeshine's workflows, moved out of `.github/workflows` so
they don't run in this fork: they trigger on every branch push and depend on
SignPath signing, self-hosted release runners and repository secrets we don't
have. They are kept here unchanged to ease merges from upstream.

The fork's own CI is `.github/workflows/build.yml`.
