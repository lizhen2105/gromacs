Performance improvements
^^^^^^^^^^^^^^^^^^^^^^^^

.. Note to developers!
   Please use """"""" to underline the individual entries for fixed issues in the subfolders,
   otherwise the formatting on the webpage is messed up.
   Also, please use the syntax :issue:`number` to reference issues on GitLab, without the
   a space between the colon and number!

Extend supported use-cases for GPU version of update and constraints
""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""

GPU version of update and constraints can now be used for FEP, except mass and constraints
free-energy perturbation.
       
Reduce time spent in grompp with large numbers of distance restraints
"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""

The time `gmx grompp` spent processing distance restraint has been
changed from quadratic in the number of restraints to linear.
       
:issue:`3457`
