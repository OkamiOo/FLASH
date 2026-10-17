# Outils d'essai, compilés à la demande et jamais livrés
add_executable(essai-appairage EXCLUDE_FROM_ALL essai_appairage.cpp)
target_link_libraries(essai-appairage PRIVATE flashcore)

# Clé et manifeste signé des mises à jour (publication)
add_executable(signer EXCLUDE_FROM_ALL signer.cpp)
target_link_libraries(signer PRIVATE flashcore)
