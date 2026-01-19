# Contributing to Satoshi Pet

Thank you for your interest in contributing to Satoshi Pet! We welcome contributions from everyone.

## Ways to Contribute

### 🐛 Bug Reports

Found a bug? Please open an issue with:
- Device hardware version (Heltec V3, etc.)
- Firmware version (from Serial output)
- Steps to reproduce
- Expected vs actual behavior
- Serial monitor output if relevant

### 💡 Feature Requests

Have an idea? Open an issue with:
- Clear description of the feature
- Use case / why it would be useful
- Any implementation ideas you have

### 🔧 Code Contributions

1. **Fork** the repository
2. **Create** a feature branch:
   ```bash
   git checkout -b feature/your-feature-name
   ```
3. **Make** your changes
4. **Test** thoroughly on actual hardware
5. **Commit** with clear messages:
   ```bash
   git commit -m "Add: description of what you added"
   git commit -m "Fix: description of what you fixed"
   ```
6. **Push** to your fork:
   ```bash
   git push origin feature/your-feature-name
   ```
7. **Open** a Pull Request

## Code Style

### C++ / Arduino

- Use 2-space indentation
- Use descriptive variable names
- Add comments for complex logic
- Keep functions focused and small
- Use `const` where applicable

### Commit Messages

- Start with a verb: Add, Fix, Update, Remove, Refactor
- Keep the first line under 50 characters
- Add details in the body if needed

### Example:

```
Add: Low battery warning animation

- Shows warning at 10% battery
- Plays alert sound (respects quiet hours)
- Auto-dismisses after 60 seconds
```

## Testing

Before submitting:

1. **Compile** without errors
2. **Flash** to actual hardware
3. **Test** the feature/fix works
4. **Verify** existing features still work
5. **Check** Serial output for errors/warnings

## Hardware Contributions

### 3D Models

- Use STL format for print files
- Include STEP files for modifications
- Document print settings
- Test fit before submitting

### Wiring/Schematics

- Use KiCad or similar open-source tools
- Include both schematic and PNG export
- Document any component substitutions

## Documentation

- Keep README.md up to date
- Add JSDoc-style comments to new functions
- Update wiring diagrams if pins change
- Add troubleshooting tips for new features

## Community Guidelines

- Be respectful and welcoming
- Help others learn
- Focus on constructive feedback
- Credit others' work appropriately

## Questions?

- Open a Discussion on GitHub
- Join our Discord (coming soon)

Thank you for making Satoshi Pet better! 🐾⚡
