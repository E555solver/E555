#!/bin/bash
#SBATCH --job-name=E555
#SBATCH --ntasks=1 --cpus-per-task=8 --mem=10G --time=12:00:00
#SBATCH --output=logs/%x_%j.out
#
# slurm_wrapper.sh -- run any E555 script under Slurm.
#
#   sbatch pipeline/slurm_wrapper.sh examples/02_finalizer_regrow.sh
#   sbatch pipeline/slurm_wrapper.sh pipeline/run_pipeline.sh THREADS=8 RUN_DIR=run1
#   sbatch --cpus-per-task=32 --mem=64G pipeline/slurm_wrapper.sh pipeline/run_pipeline.sh THREADS=32
#
# The directives above are defaults; anything passed to sbatch overrides them.
# This is the only file in the repository that knows what a scheduler is: the
# science scripts are plain bash and stay that way.
mkdir -p logs
bash "$@"
