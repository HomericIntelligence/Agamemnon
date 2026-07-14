# Gitleaks Allowlist Quarterly Audit

## Purpose

The `.gitleaks.toml` file allowlists specific paths and regex patterns that are known
to produce false positives (test fixtures, example keys, localhost URLs).  As the
repository grows, new paths or patterns may be added without review, inadvertently
silencing real secret detections.  This document prescribes the mandatory quarterly
review that keeps the allowlist accurate and minimal.

## Cadence

| Quarter | Target date |
|---------|-------------|
| Q1      | First week of January  |
| Q2      | First week of April    |
| Q3      | First week of July     |
| Q4      | First week of October  |

## Reviewer

`@mvillmow` is the designated allowlist owner (see `.github/CODEOWNERS`).  If a
dedicated security team is formed in the future, transfer ownership to
`@HomericIntelligence/security` in both `CODEOWNERS` and this document.

## Audit procedure

1. **Run gitleaks in audit mode** against the full commit history:

   ```bash
   gitleaks detect \
     --source . \
     --config .gitleaks.toml \
     --report-format json \
     --report-path /tmp/gitleaks-audit-$(date +%Y-Q%q).json \
     --no-git false
   ```

   > Tip: The monthly CI job (`.github/workflows/gitleaks-audit.yml`) uploads this
   > report as a workflow artifact — download the latest artifact instead of running
   > locally if preferred.

2. **Review the JSON report** for any findings that are suppressed by allowlist paths
   or regexes.  For each suppressed match, confirm one of:
   - It is genuinely a test fixture or generated file (path allowlist is correct).
   - It matches a well-known placeholder pattern (regex allowlist is correct).
   - It would be flagged by the base ruleset if the allowlist were removed — if so,
     open a security incident immediately rather than extending the allowlist.

3. **Prune stale entries** — if a previously allowlisted path no longer exists in the
   repository, remove it from `.gitleaks.toml`.

4. **Document findings** — open a GitHub Issue tagged `security` and `gitleaks-audit`
   summarising:
   - Number of findings reviewed
   - Any stale paths removed
   - Any new paths or patterns proposed (with justification)
   - Sign-off by the reviewer

5. **Any change to `.gitleaks.toml`** must be reviewed by the CODEOWNERS before merge.

## Automated monthly scan

`.github/workflows/gitleaks-audit.yml` runs on the 1st of each month and on every
push to `main`.  It uploads the gitleaks JSON report as a workflow artifact named
`gitleaks-report-YYYY-MM`.  Download the artifact from the Actions tab to obtain the
latest scan results without running locally.

## References

- [Gitleaks documentation](https://github.com/gitleaks/gitleaks)
- Current allowlist configuration: `.gitleaks.toml`
- Origin issue: HomericIntelligence/Agamemnon#268
- Follow-up from: HomericIntelligence/Agamemnon#66
