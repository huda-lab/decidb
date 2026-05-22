/**
 * DecidB Website - Main JavaScript
 * Handles navigation, code copying, scroll spy, and interactive elements
 */

(function() {
  'use strict';

  // ============================================
  // Theme Toggle (Dark / Light)
  // ============================================
  function initThemeToggle() {
    var toggle = document.getElementById('theme-toggle');
    if (!toggle) return;

    toggle.addEventListener('click', function() {
      // Enable smooth color transition
      document.documentElement.classList.add('theme-transition');

      var current = document.documentElement.getAttribute('data-theme') || 'light';
      var next = current === 'dark' ? 'light' : 'dark';

      document.documentElement.setAttribute('data-theme', next);
      localStorage.setItem('decidb-theme', next);

      // Remove transition class after animation completes
      setTimeout(function() {
        document.documentElement.classList.remove('theme-transition');
      }, 350);
    });
  }

  // ============================================
  // Mobile Navigation Toggle
  // ============================================
  function initMobileNav() {
    const menuButton = document.getElementById('mobile-menu-button');
    const mobileMenu = document.getElementById('mobile-menu');
    const menuOpenIcon = document.getElementById('menu-open-icon');
    const menuCloseIcon = document.getElementById('menu-close-icon');

    if (!menuButton || !mobileMenu) return;

    menuButton.addEventListener('click', function() {
      const isOpen = mobileMenu.classList.toggle('open');

      // Toggle icons
      if (menuOpenIcon && menuCloseIcon) {
        menuOpenIcon.classList.toggle('hidden', isOpen);
        menuCloseIcon.classList.toggle('hidden', !isOpen);
      }

      // Update ARIA attributes
      menuButton.setAttribute('aria-expanded', isOpen);
      mobileMenu.setAttribute('aria-hidden', !isOpen);
    });

    // Close menu when clicking outside
    document.addEventListener('click', function(event) {
      if (!menuButton.contains(event.target) && !mobileMenu.contains(event.target)) {
        mobileMenu.classList.remove('open');
        if (menuOpenIcon && menuCloseIcon) {
          menuOpenIcon.classList.remove('hidden');
          menuCloseIcon.classList.add('hidden');
        }
        menuButton.setAttribute('aria-expanded', 'false');
        mobileMenu.setAttribute('aria-hidden', 'true');
      }
    });

    // Close menu on escape key
    document.addEventListener('keydown', function(event) {
      if (event.key === 'Escape' && mobileMenu.classList.contains('open')) {
        mobileMenu.classList.remove('open');
        if (menuOpenIcon && menuCloseIcon) {
          menuOpenIcon.classList.remove('hidden');
          menuCloseIcon.classList.add('hidden');
        }
        menuButton.setAttribute('aria-expanded', 'false');
        mobileMenu.setAttribute('aria-hidden', 'true');
        menuButton.focus();
      }
    });
  }

  // ============================================
  // Copy to Clipboard for Code Blocks
  // ============================================
  function initCopyButtons() {
    const copyButtons = document.querySelectorAll('.copy-button');

    copyButtons.forEach(function(button) {
      button.addEventListener('click', async function() {
        const targetSelector = button.getAttribute('data-clipboard-target');
        const codeBlock = targetSelector
          ? document.querySelector(targetSelector)
          : button.closest('.code-block')?.querySelector('code');

        if (!codeBlock) return;

        const text = codeBlock.textContent;

        try {
          await navigator.clipboard.writeText(text);

          // Show success state
          const originalText = button.innerHTML;
          button.innerHTML = '<svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5 13l4 4L19 7"></path></svg> Copied!';
          button.classList.add('copied');

          // Reset after 2 seconds
          setTimeout(function() {
            button.innerHTML = originalText;
            button.classList.remove('copied');
          }, 2000);
        } catch (err) {
          console.error('Failed to copy text:', err);

          // Fallback for older browsers
          fallbackCopyTextToClipboard(text, button);
        }
      });
    });
  }

  function fallbackCopyTextToClipboard(text, button) {
    const textArea = document.createElement('textarea');
    textArea.value = text;
    textArea.style.cssText = 'position:fixed;left:-9999px;top:-9999px';
    document.body.appendChild(textArea);
    textArea.focus();
    textArea.select();

    try {
      document.execCommand('copy');
      const originalText = button.innerHTML;
      button.innerHTML = '<svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5 13l4 4L19 7"></path></svg> Copied!';
      button.classList.add('copied');

      setTimeout(function() {
        button.innerHTML = originalText;
        button.classList.remove('copied');
      }, 2000);
    } catch (err) {
      console.error('Fallback: Could not copy text', err);
    }

    document.body.removeChild(textArea);
  }

  // ============================================
  // Sidebar Scroll Spy
  // ============================================
  function initScrollSpy() {
    const sidebar = document.querySelector('.sidebar-nav');
    if (!sidebar) return;

    const sidebarLinks = sidebar.querySelectorAll('.sidebar-link');
    const sections = [];

    // Collect sections that correspond to sidebar links
    sidebarLinks.forEach(function(link) {
      const href = link.getAttribute('href');
      if (href && href.startsWith('#')) {
        const section = document.querySelector(href);
        if (section) {
          sections.push({ element: section, link: link });
        }
      }
    });

    if (sections.length === 0) return;

    function updateActiveLink() {
      const scrollPosition = window.scrollY + 100; // Offset for header

      // Find the current section
      let currentSection = sections[0];

      for (let i = sections.length - 1; i >= 0; i--) {
        if (sections[i].element.offsetTop <= scrollPosition) {
          currentSection = sections[i];
          break;
        }
      }

      // Update active states
      sidebarLinks.forEach(function(link) {
        link.classList.remove('active');
      });

      if (currentSection) {
        currentSection.link.classList.add('active');
      }
    }

    // Throttle scroll events
    let ticking = false;
    window.addEventListener('scroll', function() {
      if (!ticking) {
        window.requestAnimationFrame(function() {
          updateActiveLink();
          ticking = false;
        });
        ticking = true;
      }
    });

    // Initial call
    updateActiveLink();
  }

  // ============================================
  // Active Navigation Highlighting
  // ============================================
  function initActiveNavHighlight() {
    const currentPath = window.location.pathname;
    const navLinks = document.querySelectorAll('.nav-link');

    navLinks.forEach(function(link) {
      const href = link.getAttribute('href');
      if (!href) return;

      // Get the filename from the href
      const linkPath = href.split('/').pop();
      const currentFile = currentPath.split('/').pop() || 'index.html';

      if (linkPath === currentFile ||
          (linkPath === 'index.html' && (currentFile === '' || currentFile === '/'))) {
        link.classList.add('active');
      }
    });
  }

  // ============================================
  // Collapsible Sections
  // ============================================
  function initCollapsibles() {
    const collapsibles = document.querySelectorAll('.collapsible');

    collapsibles.forEach(function(collapsible) {
      const header = collapsible.querySelector('.collapsible-header');
      const content = collapsible.querySelector('.collapsible-content');

      if (!header || !content) return;

      header.addEventListener('click', function() {
        collapsible.classList.toggle('open');

        // Update ARIA attributes
        const isOpen = collapsible.classList.contains('open');
        header.setAttribute('aria-expanded', isOpen);
        content.setAttribute('aria-hidden', !isOpen);
      });

      // Handle keyboard interaction
      header.addEventListener('keydown', function(event) {
        if (event.key === 'Enter' || event.key === ' ') {
          event.preventDefault();
          header.click();
        }
      });
    });
  }

  // ============================================
  // Smooth Scroll for Anchor Links
  // ============================================
  function initSmoothScroll() {
    document.querySelectorAll('a[href^="#"]').forEach(function(anchor) {
      anchor.addEventListener('click', function(e) {
        const href = this.getAttribute('href');
        if (href === '#') return;

        const target = document.querySelector(href);
        if (target) {
          e.preventDefault();
          target.scrollIntoView({
            behavior: 'smooth',
            block: 'start'
          });

          // Update URL without jumping
          history.pushState(null, null, href);
        }
      });
    });
  }

  // ============================================
  // Installation Method Tabs (for getting-started page)
  // ============================================
  function initInstallTabs() {
    const tabs = document.querySelectorAll('.install-tab');
    const contents = document.querySelectorAll('.install-content');

    if (tabs.length === 0 || contents.length === 0) return;

    tabs.forEach(function(tab) {
      tab.addEventListener('click', function() {
        const targetId = 'install-' + this.getAttribute('data-tab');

        // Update active tab styling
        tabs.forEach(function(t) {
          t.classList.remove('active');
        });
        this.classList.add('active');

        // Show corresponding content
        contents.forEach(function(content) {
          if (content.id === targetId) {
            content.classList.remove('hidden');
          } else {
            content.classList.add('hidden');
          }
        });

        // Re-initialize Lucide icons for the newly visible content
        if (typeof lucide !== 'undefined') {
          lucide.createIcons();
        }

        // Re-highlight code blocks
        if (typeof Prism !== 'undefined') {
          Prism.highlightAll();
        }
      });
    });
  }

  // ============================================
  // Example Filter (for examples page)
  // ============================================
  function initExampleFilters() {
    const filterButtons = document.querySelectorAll('[data-filter]');
    const exampleCards = document.querySelectorAll('[data-category]');

    if (filterButtons.length === 0 || exampleCards.length === 0) return;

    filterButtons.forEach(function(button) {
      button.addEventListener('click', function() {
        const filter = this.getAttribute('data-filter');

        // Update active button
        filterButtons.forEach(function(btn) {
          btn.classList.remove('active', 'bg-success', 'text-white');
          btn.classList.add('bg-canvas-subtle', 'text-fg-muted');
        });
        this.classList.add('active', 'bg-success', 'text-white');
        this.classList.remove('bg-canvas-subtle', 'text-fg-muted');

        // Filter cards
        exampleCards.forEach(function(card) {
          const category = card.getAttribute('data-category');

          if (filter === 'all' || category === filter) {
            card.style.display = '';
            card.classList.remove('hidden');
          } else {
            card.style.display = 'none';
            card.classList.add('hidden');
          }
        });
      });
    });
  }

  // ============================================
  // Initialize Mermaid Diagrams
  // ============================================
  function initMermaid() {
    if (typeof mermaid !== 'undefined') {
      mermaid.initialize({
        startOnLoad: true,
        theme: 'default',
        securityLevel: 'loose',
        flowchart: {
          useMaxWidth: true,
          htmlLabels: true,
          curve: 'basis'
        }
      });
    }
  }

  // ============================================
  // Initialize Prism.js Code Highlighting
  // ============================================
  function initPrism() {
    if (typeof Prism !== 'undefined') {
      // Extend SQL grammar with DecidB-specific clause keywords so that
      // DECIDE, SUCH THAT, MAXIMIZE, and MINIMIZE are highlighted just like
      // standard SQL clauses (SELECT, FROM, WHERE, etc.).
      if (Prism.languages.sql) {
        var orig = Prism.languages.sql['keyword'];
        var decidbKeywords = {
          pattern: /\bDECIDE\b|\bSUCH\s+THAT\b|\bMAXIMIZE\b|\bMINIMIZE\b/i
        };
        Prism.languages.sql['keyword'] = Array.isArray(orig)
          ? [decidbKeywords].concat(orig)
          : [decidbKeywords, orig];
      }
      Prism.highlightAll();
    }
  }

  // ============================================
  // DOM Ready Handler
  // ============================================
  function onDOMReady(callback) {
    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', callback);
    } else {
      callback();
    }
  }

  // ============================================
  // Initialize All Features
  // ============================================
  onDOMReady(function() {
    initThemeToggle();
    initMobileNav();
    initCopyButtons();
    initScrollSpy();
    initActiveNavHighlight();
    initCollapsibles();
    initSmoothScroll();
    initInstallTabs();
    initExampleFilters();
    initMermaid();
    initPrism();

    console.log('DecidB website initialized');
  });

})();
