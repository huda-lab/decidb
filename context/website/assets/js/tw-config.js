/**
 * DeciDB Website — Tailwind CDN semantic color config
 * Maps semantic class names (bg-canvas, text-fg, border-bdcolor, etc.)
 * to CSS custom properties defined in custom.css.
 * Must be loaded BEFORE the Tailwind CDN script.
 */
tailwind.config = {
  theme: {
    extend: {
      colors: {
        canvas:          'var(--color-canvas)',
        'canvas-subtle': 'var(--color-canvas-subtle)',
        'canvas-inset':  'var(--color-canvas-inset)',
        fg:              'var(--color-fg-default)',
        'fg-muted':      'var(--color-fg-muted)',
        'fg-subtle':     'var(--color-fg-subtle)',
        accent:          'var(--color-accent-fg)',
        'accent-emphasis': 'var(--color-accent-emphasis)',
        success:         'var(--color-success-fg)',
        warning:         'var(--color-warning-fg)',
        danger:          'var(--color-danger-fg)',
        bdcolor:         'var(--color-border-default)',
        'bdcolor-muted': 'var(--color-border-muted)',
      },
      borderColor: {
        bdcolor:         'var(--color-border-default)',
        'bdcolor-muted': 'var(--color-border-muted)',
        accent:          'var(--color-accent-fg)',
        success:         'var(--color-success-fg)',
        danger:          'var(--color-danger-fg)',
      },
      ringColor: {
        accent:          'var(--color-accent-fg)',
      },
      gradientColorStops: {
        canvas:          'var(--color-canvas)',
        'canvas-subtle': 'var(--color-canvas-subtle)',
        'canvas-inset':  'var(--color-canvas-inset)',
      },
      placeholderColor: {
        'fg-subtle':     'var(--color-fg-subtle)',
      },
    },
  },
};
