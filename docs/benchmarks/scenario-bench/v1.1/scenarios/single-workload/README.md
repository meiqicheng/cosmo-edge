# Single-task reproduction descriptors

- `person-detector.public.yml`: person detection at 24, 10, 7, and 5 FPS.
- `no-safety-helmet-analysis.public.yml`: two-stage detector-plus-classifier analysis at the same FPS gradient.

Each step holds for 30 seconds and adds one channel. Resolve the model reference and device-local task layout before execution.
