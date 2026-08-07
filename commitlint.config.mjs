// Repo-wide commit message policy. Mirrors web/commitlint.config.mjs so the
// same rules apply to Go/C++/Python/docs commits, not just the web subtree.
// Enforced in CI via .github/workflows/commitlint.yml.
export default {
  extends: ['@commitlint/config-conventional'],
  rules: {
    'type-enum': [
      2,
      'always',
      [
        'feat',      // new feature
        'fix',       // bug fix
        'docs',      // documentation
        'style',     // code style
        'refactor',  // refactoring
        'perf',      // performance
        'test',      // tests
        'chore',     // build / auxiliary
        'ci',        // CI/CD
        'build',     // build system
        'revert',    // revert
      ],
    ],
    'type-case': [2, 'always', 'lower-case'],
    'type-empty': [2, 'never'],
    'subject-case': [2, 'always', 'lower-case'],
    'subject-empty': [2, 'never'],
    'subject-full-stop': [2, 'never', '.'],
    'header-max-length': [2, 'always', 72],
    // Body lines are NOT wrapped to 100 chars. The inherited
    // config-conventional `body-max-line-length: 100` is disabled so commit
    // bodies can be natural multi-line prose (user preference 2026-07-30).
    'body-max-line-length': [0, 'always', Infinity],
  },
};
