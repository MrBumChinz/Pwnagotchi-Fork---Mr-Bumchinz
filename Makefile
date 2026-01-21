.PHONY: help install clean test lint

help:
	@echo "Pwnagotchi Build Commands"
	@echo "========================="
	@echo "make install     - Install pwnagotchi and dependencies"
	@echo "make clean       - Clean build artifacts"
	@echo "make test        - Run tests"
	@echo "make lint        - Run code linting"
	@echo "make dev         - Install in development mode"

install:
	pip install -e .

dev:
	pip install -e ".[dev]"

clean:
	find . -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
	find . -type f -name "*.pyc" -delete
	rm -rf build/ dist/ *.egg-info/ 2>/dev/null || true

test:
	pytest tests/ -v

lint:
	black --check pwnagotchi/
	pylint pwnagotchi/

format:
	black pwnagotchi/

.DEFAULT_GOAL := help
