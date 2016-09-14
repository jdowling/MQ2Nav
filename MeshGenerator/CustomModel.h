#pragma once

#include "EQConfig.h"
#include "ModelEntry.h"
#include <memory>


class CustomModel
{
public:
	CustomModel(const std::shared_ptr<ModelEntry> model, const std::string& name);
	~CustomModel();

	void writeToFile();

private:
	EQConfig config;
	std::shared_ptr<ModelEntry> model;
	const std::string& name;

	std::string getModelFileName();
};