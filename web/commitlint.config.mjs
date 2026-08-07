export default {
  extends: ['@commitlint/config-conventional'],
  rules: {
    'type-enum': [
      2,
      'always',
      [
        'feat', // New feature
        'fix', // Bug fix
        'docs', // Documentation update
        'style', // Code style adjustment
        'refactor', // Refactoring
        'perf', // Performance optimization
        'test', // Test related
        'chore', // Build process or auxiliary tool changes
        'ci', // CI/CD related
        'build', // Build related
        'revert', // Revert
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
