Step 11: Adding Export Configuration
====================================

During :guide:`tutorial/Installing and Testing` of the tutorial we added the
ability for CMake to install the library and headers of the project. During
:guide:`tutorial/Packaging an Installer` we added the ability to package up
this information so it could be distributed to other people.

The next step is to add the necessary information so that other CMake projects
can use our project, be it from a build directory, a local install or when
packaged.

The first step is to update our :command:`install(TARGETS)` commands to not
only specify a ``DESTINATION`` but also an ``EXPORT``. The ``EXPORT`` keyword
generates and installs a CMake file containing code to import all targets
listed in the install command from the installation tree. So let's go ahead and
explicitly ``EXPORT`` the ``MathFunctions`` library by updating the ``install``
command in ``MathFunctions/CMakeLists.txt`` to look like:

.. literalinclude:: Complete/MathFunctions/CMakeLists.txt
  :caption: MathFunctions/CMakeLists.txt
  :language: cmake
  :start-after: # install rules

Now that we have ``MathFunctions`` being exported, we also need to explicitly
install the generated ``MathFunctionsTargets.cmake`` file. This is done by
adding the following to the bottom of the top-level ``CMakeLists.txt``:

.. literalinclude:: Complete/CMakeLists.txt
  :caption: CMakeLists.txt
  :language: cmake
  :start-after: # install the configuration targets
  :end-before: include(CMakePackageConfigHelpers)

At this point you should try and run CMake. If everything is setup properly
you will see that CMake will generate an error that looks like:

.. code-block:: console

  Target "MathFunctions" INTERFACE_INCLUDE_DIRECTORIES property contains
  path:

    "/Users/robert/Documents/CMakeClass/Tutorial/Step11/MathFunctions"

  which is prefixed in the source directory.

What CMake is trying to say is that during generating the export information
it will export a path that is intrinsically tied to the current machine and
will not be valid on other machines. The solution to this is to update the
``MathFunctions`` :command:`target_include_directories` to understand that it
needs different ``INTERFACE`` locations when being used from within the build
directory and from an install / package. This means converting the
:command:`target_include_directories` call for ``MathFunctions`` to look like:

.. literalinclude:: Step12/MathFunctions/CMakeLists.txt
  :caption: MathFunctions/CMakeLists.txt
  :language: cmake
  :start-after: # to find MathFunctions.h, while we don't.
  :end-before: # should we use our own math functions

Once this has been updated, we can re-run CMake and verify that it doesn't
warn anymore.

At this point, we have CMake properly packaging the target information that is
required but we will still need to generate a ``MathFunctionsConfig.cmake`` so
that the CMake :command:`find_package` command can find our project. So let's go
ahead and add a new file to the top-level of the project called
``Config.cmake.in`` with the following contents:

.. literalinclude:: Step12/Config.cmake.in
  :caption: Config.cmake.in

Then, to properly configure and install that file, add the following to the
bottom of the top-level ``CMakeLists.txt``:

.. literalinclude:: Step12/CMakeLists.txt
  :caption: CMakeLists.txt
  :language: cmake
  :start-after: # install the configuration targets
  :end-before: # generate the export

At this point, we have generated a relocatable CMake Configuration for our
project that can be used after the project has been installed or packaged. If
we want our project to also be used from a build directory we only have to add
the following to the bottom of the top level ``CMakeLists.txt``:

.. literalinclude:: Step12/CMakeLists.txt
  :caption: CMakeLists.txt
  :language: cmake
  :start-after: # needs to be after the install(TARGETS ) command

With this export call we now generate a ``Targets.cmake``, allowing the
configured ``MathFunctionsConfig.cmake`` in the build directory to be used by
other projects, without needing it to be installed.
