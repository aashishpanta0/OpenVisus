
add_subdirectory(Kernel)
add_subdirectory(Db)

if (VISUS_DATAFLOW)

	add_subdirectory(Dataflow)
	add_subdirectory(Nodes)

	if (VISUS_GUI)
		add_subdirectory(Gui)
	endif()
	
endif()

if (VISUS_PYTHON)
	add_subdirectory(swig)
endif()


if(VISUS_WEAVING)
	add_subdirectory(VisusWeaving)
endif()	
